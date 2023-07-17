//===--- ComputeSideEffects.swift ------------------------------------------==//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

/// Computes function side effects.
///
/// Computes the `SideEffects` for a function, which consists of argument- and global
/// effects.
/// For example, if a function writes to the first argument and reads from a global variable,
/// the side effects
/// ```
///   [%0: write v**]
///   [global: read]
/// ```
/// are computed.
///
let computeSideEffects = FunctionPass(name: "compute-side-effects", {
  (function: Function, context: FunctionPassContext) in

  if function.isAvailableExternally {
    // We cannot assume anything about function, which are defined in another module,
    // even if the serialized SIL of its body is available in the current module.
    // If the other module was compiled with library evolution, the implementation
    // (and it's effects) might change in future versions of the other module/library.
    //
    // TODO: only do this for functions which are de-serialized from library-evolution modules.
    return
  }

  if function.effectAttribute != .none {
    // Don't try to infer side effects if there are defined effect attributes.
    return
  }

  var collectedEffects = CollectedEffects(function: function, context)

  // First step: collect effects from all instructions.
  //
  for block in function.blocks {
    for inst in block.instructions {
      collectedEffects.addInstructionEffects(inst)
    }
  }

  // Second step: If an argument has unknown uses, we must add all previously collected
  // global effects to the argument, because we don't know to which "global" side-effect
  // instruction the argument might have escaped.
  for argument in function.arguments {
    collectedEffects.addEffectsForEcapingArgument(argument: argument)
  }

  // Don't modify the effects if they didn't change. This avoids sending a change notification
  // which can trigger unnecessary other invalidations.
  if let existingEffects = function.effects.sideEffects,
     existingEffects.arguments == collectedEffects.argumentEffects,
     existingEffects.global == collectedEffects.globalEffects {
    return
  }

  // Finally replace the function's side effects.
  context.modifyEffects(in: function) { (effects: inout FunctionEffects) in
    effects.sideEffects = SideEffects(arguments: collectedEffects.argumentEffects, global: collectedEffects.globalEffects)
  }
})

/// The collected argument and global side effects of the function.
private struct CollectedEffects {

  private let context: FunctionPassContext
  private let calleeAnalysis: CalleeAnalysis

  private(set) var argumentEffects: [SideEffects.ArgumentEffects]
  private(set) var globalEffects = SideEffects.GlobalEffects()

  init(function: Function, _ context: FunctionPassContext) {
    self.context = context
    self.calleeAnalysis = context.calleeAnalysis
    self.argumentEffects = Array(repeating: SideEffects.ArgumentEffects(), count: function.entryBlock.arguments.count)
  }

  mutating func addInstructionEffects(_ inst: Instruction) {
    var checkedIfDeinitBarrier = false
    switch inst {
    case is CopyValueInst, is RetainValueInst, is StrongRetainInst:
      addEffects(.copy, to: inst.operands[0].value, fromInitialPath: SmallProjectionPath(.anyValueFields))

    case is DestroyValueInst, is ReleaseValueInst, is StrongReleaseInst:
      addDestroyEffects(ofValue: inst.operands[0].value)

    case let da as DestroyAddrInst:
      addDestroyEffects(ofAddress: da.destroyedAddress)

    case let copy as CopyAddrInst:
      addEffects(.read, to: copy.source)
      addEffects(.write, to: copy.destination)

      if !copy.isTakeOfSrc {
        addEffects(.copy, to: copy.source)
      }
      if !copy.isInitializationOfDest {
        addDestroyEffects(ofAddress: copy.destination)
      }

    case let store as StoreInst:
      addEffects(.write, to: store.destination)
      if store.storeOwnership == .assign {
        addDestroyEffects(ofAddress: store.destination)
      }

    case let store as StoreWeakInst:
      addEffects(.write, to: store.destination)

    case let store as StoreUnownedInst:
      addEffects(.write, to: store.destination)

    case is LoadInst, is LoadWeakInst, is LoadUnownedInst, is LoadBorrowInst:
      let addr = inst.operands[0].value
      addEffects(.read, to: addr)

    case let apply as FullApplySite:
      if apply.callee.type.isCalleeConsumedFunction {
        addEffects(.destroy, to: apply.callee)
        globalEffects = .worstEffects
      }
      handleApply(apply)
      checkedIfDeinitBarrier = true

    case let pa as PartialApplyInst:
      if pa.canBeAppliedInFunction(context) {
        // Only if the created closure can actually be called in the function
        // we have to consider side-effects within the closure.
        handleApply(pa)
        checkedIfDeinitBarrier = true
      }

    case let fl as FixLifetimeInst:
      // A fix_lifetime instruction acts like a read on the operand to prevent
      // releases moving above the fix_lifetime.
      addEffects(.read, to: fl.operand.value)

      // Instructions which have effects defined in SILNodes.def, but those effects are
      // not relevant for our purpose.
      // In most cases these conservative effects are there to prevent code re-scheduling within
      // the function. But this is not relevant for side effect summaries which we compute here.
    case is DeallocStackInst, is DeallocStackRefInst,
      is BeginAccessInst, is EndAccessInst,
      is BeginBorrowInst, is EndBorrowInst,
      is DebugValueInst, is KeyPathInst, is FixLifetimeInst,
      is EndApplyInst, is AbortApplyInst,
      is EndCOWMutationInst, is UnconditionalCheckedCastInst,
      is CondFailInst:
      break

    case is BeginCOWMutationInst, is IsUniqueInst, is IsEscapingClosureInst:
      // Model reference count reading as "destroy" for now. Although we could intoduce a "read-refcount"
      // effect, it would not give any significant benefit in any of our current optimizations.
      addEffects(.destroy, to: inst.operands[0].value, fromInitialPath: SmallProjectionPath(.anyValueFields))

    default:
      if inst.mayRelease {
        globalEffects = .worstEffects
      }
      if inst.mayReadFromMemory {
        globalEffects.memory.read = true
      }
      if inst.mayWriteToMemory {
        globalEffects.memory.write = true
      }
      if inst.hasUnspecifiedSideEffects {
        globalEffects.ownership.copy = true
      }

      // Ignore "local" allocations, which don't escape. They cannot be observed
      // from outside the function.
      if let alloc = inst as? Allocation, !(inst is AllocStackInst),
         alloc.isEscaping(context) {
        globalEffects.allocates = true
      }
    }
    // If we didn't already, check whether the instruction could be a deinit
    // barrier.  If it's an apply of some sort, that was already done in
    // handleApply.
    if !checkedIfDeinitBarrier,
       inst.mayBeDeinitBarrierNotConsideringSideEffects {
      globalEffects.isDeinitBarrier = true
    }
  }
  
  mutating func addEffectsForEcapingArgument(argument: FunctionArgument) {
    var escapeWalker = ArgumentEscapingWalker()

    if escapeWalker.hasUnknownUses(argument: argument) {
      // Worst case: we don't know anything about how the argument escapes.
      addEffects(globalEffects.restrictedTo(argument: argument.at(SmallProjectionPath(.anything)),
                                            withConvention: argument.convention), to: argument)

    } else if escapeWalker.foundTakingLoad {
      // In most cases we can just ignore loads. But if the load is actually "taking" the
      // underlying memory allocation, we must consider this as a "destroy", because we don't
      // know what's happening with the loaded value. If there is any destroying instruction in the
      // function, it might be the destroy of the loaded value.
      let effects = SideEffects.GlobalEffects(ownership: globalEffects.ownership)
      addEffects(effects.restrictedTo(argument: argument.at(SmallProjectionPath(.anything)),
                                      withConvention: argument.convention), to: argument)

    } else if escapeWalker.foundConsumingPartialApply && globalEffects.ownership.destroy {
      // Similar situation with apply instructions which consume the callee closure.
      addEffects(.destroy, to: argument)
    }
  }
  
  private mutating func handleApply(_ apply: ApplySite) {
    let callees = calleeAnalysis.getCallees(callee: apply.callee)
    let args = apply.arguments.enumerated().lazy.map {
      (calleeArgumentIndex: apply.calleeArgIndex(callerArgIndex: $0.0),
       callerArgument: $0.1)
    }
    addEffects(ofFunctions: callees, withArguments: args)
  }

  private mutating func addDestroyEffects(ofValue value: Value) {
    // First thing: add the destroy effect itself.
    addEffects(.destroy, to: value)

    if value.type.isClass {
      // Treat destroying a class value just like a call to it's destructor(s).
      let destructors = calleeAnalysis.getDestructors(of: value.type)
      let theSelfArgument = CollectionOfOne((calleeArgumentIndex: 0, callerArgument: value))
      addEffects(ofFunctions: destructors, withArguments: theSelfArgument)
    } else {
      // TODO: dig into the type and check for destructors of individual class fields
      addEffects(.worstEffects, to: value)
      globalEffects = .worstEffects
    }
  }

  private mutating func addDestroyEffects(ofAddress address: Value) {
    // First thing: add the destroy effect itself.
    addEffects(.destroy, to: address)

    // A destroy also involves a read from the address.
    // E.g. a `destroy_addr` is equivalent to a `%x = load [take]` and `destroy_value %x`.
    addEffects(.read, to: address)
    // Conceptually, it's also a write, because the stored value is not available anymore after the destroy
    addEffects(.write, to: address)

    // Second: add all effects of (potential) destructors which might be called if the destroy deallocates an object.
    // Note that we don't need to add any effects specific to the `address`, because the memory location is not
    // affected by a destructor of the stored value (and effects don't include anything which is loaded from memory).
    if let destructors = calleeAnalysis.getDestructors(of: address.type) {
      for destructor in destructors {
        globalEffects.merge(with: destructor.getSideEffects())
      }
    } else {
      globalEffects = .worstEffects
    }
  }

  private mutating func addEffects<Arguments: Sequence>(ofFunctions callees: FunctionArray?,
                                                        withArguments arguments: Arguments)
                                   where Arguments.Element == (calleeArgumentIndex: Int, callerArgument: Value) {
    guard let callees = callees else {
      // We don't know which function(s) are called.
      globalEffects = .worstEffects
      for (_, argument) in arguments {
        addEffects(.worstEffects, to: argument)
      }
      return
    }
    for callee in callees {
      if let sideEffects = callee.effects.sideEffects {
        globalEffects.merge(with: sideEffects.global)
      } else {
        // The callee doesn't have any computed effects. At least we can do better
        // if it has any defined effect attribute (like e.g. `[readnone]`).
        globalEffects.merge(with: callee.definedGlobalEffects)
      }
    }

    for (calleeArgIdx, argument) in arguments {
      for callee in callees {
        if let sideEffects = callee.effects.sideEffects {
          let calleeEffect = sideEffects.getArgumentEffects(for: calleeArgIdx)

          // Merge the callee effects into this function's effects
          if let calleePath = calleeEffect.read    { addEffects(.read,    to: argument, fromInitialPath: calleePath) }
          if let calleePath = calleeEffect.write   { addEffects(.write,   to: argument, fromInitialPath: calleePath) }
          if let calleePath = calleeEffect.copy    { addEffects(.copy,    to: argument, fromInitialPath: calleePath) }
          if let calleePath = calleeEffect.destroy { addEffects(.destroy, to: argument, fromInitialPath: calleePath) }
        } else {
          let convention = callee.getArgumentConvention(for: calleeArgIdx)
          let wholeArgument = argument.at(defaultPath(for: argument))
          let calleeEffects = callee.getSideEffects(forArgument: wholeArgument,
                                                    atIndex: calleeArgIdx,
                                                    withConvention: convention)
          addEffects(calleeEffects.restrictedTo(argument: wholeArgument, withConvention: convention), to: argument)
        }
      }
    }
  }

  /// Adds effects to a specific value.
  ///
  /// If the value comes from an argument (or mutliple arguments), then the effects are added
  /// to the corrseponding `argumentEffects`. Otherwise they are added to the `global` effects.
  private mutating func addEffects(_ effects: SideEffects.GlobalEffects, to value: Value) {
    addEffects(effects, to: value, fromInitialPath: defaultPath(for: value))
  }

  private mutating func addEffects(_ effects: SideEffects.GlobalEffects, to value: Value,
                                   fromInitialPath: SmallProjectionPath) {

    /// Collects the (non-address) roots of a value.
    struct GetRootsWalker : ValueUseDefWalker {
      // All function-argument roots of the value, including the path from the arguments to the values.
      var roots: Stack<(FunctionArgument, SmallProjectionPath)>

      // True, if the value has at least one non function-argument root.
      var nonArgumentRootsFound = false

      var walkUpCache = WalkerCache<SmallProjectionPath>()

      init(_ context: FunctionPassContext) {
        self.roots = Stack(context)
      }

      mutating func rootDef(value: Value, path: SmallProjectionPath) -> WalkResult {
        if let arg = value as? FunctionArgument {
          roots.push((arg, path))
        } else if value is Allocation {
          // Ignore effects on local allocations - even if those allocations escape.
          // Effects on local (potentially escaping) allocations cannot be relevant in the caller.
          return .continueWalk
        } else {
          nonArgumentRootsFound = true
        }
        return .continueWalk
      }
    }

    var findRoots = GetRootsWalker(context)
    if value.type.isAddress {
      let accessPath = value.getAccessPath(fromInitialPath: fromInitialPath)
      switch accessPath.base {
        case .stack:
          // We don't care about read and writes from/to stack locations (because they are
          // not observable from outside the function). But we need to consider copies and destroys.
          // For example, an argument could be "moved" to a stack location, which is eventually destroyed.
          // In this case it's in fact the original argument value which is destroyed.
          globalEffects.ownership.merge(with: effects.ownership)
          return
        case .argument(let arg):
          // The `value` is an address projection of an indirect argument.
          argumentEffects[arg.index].merge(effects, with: accessPath.projectionPath)
          return
        default:
          // Handle address `value`s which are are field projections from class references in direct arguments.
          if !findRoots.visitAccessStorageRoots(of: accessPath) {
            findRoots.nonArgumentRootsFound = true
          }
      }
    } else {
      _ = findRoots.walkUp(value: value, path: fromInitialPath)
    }
    // Because of phi-arguments, a single (non-address) `value` can come from multiple arguments.
    while let (arg, path) = findRoots.roots.pop() {
      argumentEffects[arg.index].merge(effects, with: path)
    }
    if findRoots.nonArgumentRootsFound {
      // The `value` comes from some non-argument root, e.g. a load instruction.
      globalEffects.merge(with: effects)
    }
  }
}

private func defaultPath(for value: Value) -> SmallProjectionPath {
  if value.type.isAddress {
    return SmallProjectionPath(.anyValueFields)
  }
  if value.type.isClass {
    return SmallProjectionPath(.anyValueFields).push(.anyClassField)
  }
  return SmallProjectionPath(.anyValueFields).push(.anyClassField).push(.anyValueFields)
}

/// Checks if an argument escapes to some unknown user.
private struct ArgumentEscapingWalker : ValueDefUseWalker, AddressDefUseWalker {
  var walkDownCache = WalkerCache<UnusedWalkingPath>()

  /// True if the argument escapes to a load which (potentially) "takes" the memory location.
  private(set) var foundTakingLoad = false

  /// True, if the argument escapes to a closure context which might be destroyed when called.
  private(set) var foundConsumingPartialApply = false

  mutating func hasUnknownUses(argument: FunctionArgument) -> Bool {
    if argument.type.isAddress {
      return walkDownUses(ofAddress: argument, path: UnusedWalkingPath()) == .abortWalk
    } else if argument.hasTrivialNonPointerType {
      return false
    } else {
      return walkDownUses(ofValue: argument, path: UnusedWalkingPath()) == .abortWalk
    }
  }

  mutating func leafUse(value: Operand, path: UnusedWalkingPath) -> WalkResult {
    switch value.instruction {
    case is RefTailAddrInst, is RefElementAddrInst, is ProjectBoxInst:
      return walkDownUses(ofAddress: value.instruction as! SingleValueInstruction, path: path)

    // Warning: all instruction listed here, must also be handled in `CollectedEffects.addInstructionEffects`
    case is CopyValueInst, is RetainValueInst, is StrongRetainInst,
         is DestroyValueInst, is ReleaseValueInst, is StrongReleaseInst,
         is DebugValueInst, is UnconditionalCheckedCastInst,
         is ReturnInst:
      return .continueWalk

    case let apply as ApplySite:
      if apply.isCalleeOperand(value) {
        // `CollectedEffects.handleApply` only handles argument operands of an apply, but not the callee operand.
        return .abortWalk
      }
      if let pa = apply as? PartialApplyInst, !pa.isOnStack {
        foundConsumingPartialApply = true
      }
      return .continueWalk
    default:
      return .abortWalk
    }
  }

  mutating func leafUse(address: Operand, path: UnusedWalkingPath) -> WalkResult {
    let inst = address.instruction
    let function = inst.parentFunction
    switch inst {
    case let copy as CopyAddrInst:
      if address == copy.sourceOperand &&
          !address.value.hasTrivialType &&
          (!function.hasOwnership || copy.isTakeOfSrc) {
        foundTakingLoad = true
      }
      return .continueWalk
 
    case let load as LoadInst:
      if !address.value.hasTrivialType &&
          // In non-ossa SIL we don't know if a load is taking.
          (!function.hasOwnership || load.loadOwnership == .take) {
        foundTakingLoad = true
      }
      return .continueWalk

    case is LoadWeakInst, is LoadUnownedInst, is LoadBorrowInst:
      if !function.hasOwnership && !address.value.hasTrivialType {
        foundTakingLoad = true
      }
      return .continueWalk

    // Warning: all instruction listed here, must also be handled in `CollectedEffects.addInstructionEffects`
    case is StoreInst, is StoreWeakInst, is StoreUnownedInst, is ApplySite, is DestroyAddrInst,
         is DebugValueInst:
      return .continueWalk

    default:
      return .abortWalk
    }
  }
}

private extension SideEffects.GlobalEffects {
  static var read: Self    { Self(memory: SideEffects.Memory(read: true)) }
  static var write: Self   { Self(memory: SideEffects.Memory(write: true)) }
  static var copy: Self    { Self(ownership: SideEffects.Ownership(copy: true)) }
  static var destroy: Self { Self(ownership: SideEffects.Ownership(destroy: true)) }
}

private extension SideEffects.ArgumentEffects {
  mutating func merge(_ effects: SideEffects.GlobalEffects, with path: SmallProjectionPath) {
    if effects.memory.read       { read.merge(with: path) }
    if effects.memory.write      { write.merge(with: path) }
    if effects.ownership.copy    { copy.merge(with: path) }
    if effects.ownership.destroy { destroy.merge(with: path) }
  }
}

private extension PartialApplyInst {
  func canBeAppliedInFunction(_ context: FunctionPassContext) -> Bool {
    struct EscapesToApply : EscapeVisitor {
      func visitUse(operand: Operand, path: EscapePath) -> UseResult {
        switch operand.instruction {
        case is FullApplySite:
          // Any escape to apply - regardless if it's an argument or the callee operand - might cause
          // the closure to be called.
          return .abort
        case is ReturnInst:
          return .ignore
        default:
          return .continueWalk
        }
      }
      var followTrivialTypes: Bool { true }
    }

    return self.isEscapingWhenWalkingDown(using: EscapesToApply(), context)
  }
}
