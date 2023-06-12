//===--- Context.swift - defines the context types ------------------------===//
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
import OptimizerBridging

/// The base type of all contexts.
protocol Context {
  var _bridged: BridgedPassContext { get }
}

extension Context {
  var options: Options { Options(_bridged: _bridged) }

  // The calleeAnalysis is not specific to a function and therefore can be provided in
  // all contexts.
  var calleeAnalysis: CalleeAnalysis {
    let bridgeCA = _bridged.getCalleeAnalysis()
    return CalleeAnalysis(bridged: bridgeCA)
  }
}

/// A context which allows mutation of a function's SIL.
protocol MutatingContext : Context {
  // Called by all instruction mutations, including inserted new instructions.
  var notifyInstructionChanged: (Instruction) -> () { get }
}

extension MutatingContext {
  func notifyInvalidatedStackNesting() { _bridged.notifyInvalidatedStackNesting() }
  var needFixStackNesting: Bool { _bridged.getNeedFixStackNesting() }

  /// Splits the basic block, which contains `inst`, before `inst` and returns the
  /// new block.
  ///
  /// `inst` and all subsequent instructions are moved to the new block, while all
  /// instructions _before_ `inst` remain in the original block.
  func splitBlock(at inst: Instruction) -> BasicBlock {
    notifyBranchesChanged()
    return _bridged.splitBlock(inst.bridged).block
  }

  func erase(instruction: Instruction) {
    if instruction is FullApplySite {
      notifyCallsChanged()
    }
    if instruction is TermInst {
      notifyBranchesChanged()
    }
    notifyInstructionsChanged()

    _bridged.eraseInstruction(instruction.bridged)
  }

  func erase(instructionIncludingDebugUses inst: Instruction) {
    for result in inst.results {
      for use in result.uses {
        assert(use.instruction is DebugValueInst, "instruction to delete may only have debug_value uses")
        erase(instruction: use.instruction)
      }
    }
    erase(instruction: inst)
  }

  func tryOptimizeApplyOfPartialApply(closure: PartialApplyInst) -> Bool {
    if _bridged.tryOptimizeApplyOfPartialApply(closure.bridged) {
      notifyInstructionsChanged()
      notifyCallsChanged()

      for use in closure.callee.uses {
        if use.instruction is FullApplySite {
          notifyInstructionChanged(use.instruction)
        }
      }
      return true
    }
    return false
  }

  func tryDeleteDeadClosure(closure: SingleValueInstruction, needKeepArgsAlive: Bool = true) -> Bool {
    if _bridged.tryDeleteDeadClosure(closure.bridged, needKeepArgsAlive) {
      notifyInstructionsChanged()
      return true
    }
    return false
  }

  func tryDevirtualize(apply: FullApplySite, isMandatory: Bool) -> ApplySite? {
    let result = _bridged.tryDevirtualizeApply(apply.bridged, isMandatory)
    if let newApply = result.newApply.instruction {
      erase(instruction: apply)
      notifyInstructionsChanged()
      notifyCallsChanged()
      if result.cfgChanged {
        notifyBranchesChanged()
      }
      notifyInstructionChanged(newApply)
      return newApply as! FullApplySite
    }
    return nil
  }

  func inlineFunction(apply: FullApplySite, mandatoryInline: Bool) {
    let instAfterInling: Instruction?
    switch apply {
    case is ApplyInst, is BeginApplyInst:
      instAfterInling = apply.next
    case is TryApplyInst:
      instAfterInling = apply.parentBlock.next?.instructions.first
    default:
      instAfterInling = nil
    }

    _bridged.inlineFunction(apply.bridged, mandatoryInline)

    if let instAfterInling = instAfterInling {
      notifyNewInstructions(from: apply, to: instAfterInling)
    }
  }

  private func notifyNewInstructions(from: Instruction, to: Instruction) {
    var inst = from
    while inst != to {
      if !inst.isDeleted {
        notifyInstructionChanged(inst)
      }
      if let next = inst.next {
        inst = next
      } else {
        inst = inst.parentBlock.next!.instructions.first!
      }
    }
  }

  func getContextSubstitutionMap(for type: Type) -> SubstitutionMap {
    SubstitutionMap(_bridged.getContextSubstitutionMap(type.bridged))
  }

  // Private utilities

  fileprivate func notifyInstructionsChanged() {
    _bridged.asNotificationHandler().notifyChanges(.instructionsChanged)
  }

  fileprivate func notifyCallsChanged() {
    _bridged.asNotificationHandler().notifyChanges(.callsChanged)
  }

  fileprivate func notifyBranchesChanged() {
    _bridged.asNotificationHandler().notifyChanges(.branchesChanged)
  }
}

/// The context which is passed to the run-function of a FunctionPass.
struct FunctionPassContext : MutatingContext {
  let _bridged: BridgedPassContext

  // A no-op.
  var notifyInstructionChanged: (Instruction) -> () { return { inst in } }

  func continueWithNextSubpassRun(for inst: Instruction? = nil) -> Bool {
    return _bridged.continueWithNextSubpassRun(inst.bridged)
  }

  func createSimplifyContext(preserveDebugInfo: Bool, notifyInstructionChanged: @escaping (Instruction) -> ()) -> SimplifyContext {
    SimplifyContext(_bridged: _bridged, notifyInstructionChanged: notifyInstructionChanged, preserveDebugInfo: preserveDebugInfo)
  }

  var aliasAnalysis: AliasAnalysis {
    let bridgedAA = _bridged.getAliasAnalysis()
    return AliasAnalysis(bridged: bridgedAA)
  }

  var deadEndBlocks: DeadEndBlocksAnalysis {
    let bridgeDEA = _bridged.getDeadEndBlocksAnalysis()
    return DeadEndBlocksAnalysis(bridged: bridgeDEA)
  }

  var dominatorTree: DominatorTree {
    let bridgedDT = _bridged.getDomTree()
    return DominatorTree(bridged: bridgedDT)
  }

  var postDominatorTree: PostDominatorTree {
    let bridgedPDT = _bridged.getPostDomTree()
    return PostDominatorTree(bridged: bridgedPDT)
  }

  func loadFunction(name: StaticString, loadCalleesRecursively: Bool) -> Function? {
    return name.withUTF8Buffer { (nameBuffer: UnsafeBufferPointer<UInt8>) in
      let nameStr = llvm.StringRef(nameBuffer.baseAddress, nameBuffer.count)
      return _bridged.loadFunction(nameStr, loadCalleesRecursively).function
    }
  }

  func loadFunction(function: Function, loadCalleesRecursively: Bool) -> Bool {
    if function.isDefinition {
      return true
    }
    _bridged.loadFunction(function.bridged, loadCalleesRecursively)
    return function.isDefinition
  }

  /// Looks up a function in the `Swift` module.
  /// The `name` is the source name of the function and not the mangled name.
  /// Returns nil if no such function or multiple matching functions are found.
  func lookupStdlibFunction(name: StaticString) -> Function? {
    return name.withUTF8Buffer { (nameBuffer: UnsafeBufferPointer<UInt8>) in
      let nameStr = llvm.StringRef(nameBuffer.baseAddress, nameBuffer.count)
      return _bridged.lookupStdlibFunction(nameStr).function
    }
  }

  func erase(block: BasicBlock) {
    _bridged.eraseBlock(block.bridged)
  }

  func modifyEffects(in function: Function, _ body: (inout FunctionEffects) -> ()) {
    notifyEffectsChanged()
    function._modifyEffects(body)
  }

  fileprivate func notifyEffectsChanged() {
    _bridged.asNotificationHandler().notifyChanges(.effectsChanged)
  }

  func optimizeMemoryAccesses(in function: Function) -> Bool {
    if swift.optimizeMemoryAccesses(function.bridged.getFunction()) {
      notifyInstructionsChanged()
      return true
    }
    return false
  }

  func eliminateDeadAllocations(in function: Function) -> Bool {
    if swift.eliminateDeadAllocations(function.bridged.getFunction()) {
      notifyInstructionsChanged()
      return true
    }
    return false
  }

  func specializeApplies(in function: Function, isMandatory: Bool) -> Bool {
    if _bridged.specializeAppliesInFunction(function.bridged, isMandatory) {
      notifyInstructionsChanged()
      notifyCallsChanged()
      return true
    }
    return false
  }

  func mangleOutlinedVariable(from function: Function) -> String {
    let stdString = _bridged.mangleOutlinedVariable(function.bridged)
    return String(_cxxString: stdString)
  }

  func createGlobalVariable(name: String, type: Type, isPrivate: Bool) -> GlobalVariable {
    let gv = name._withStringRef {
      _bridged.createGlobalVariable($0, type.bridged, isPrivate)
    }
    return gv.globalVar
  }
}

struct SimplifyContext : MutatingContext {
  let _bridged: BridgedPassContext
  let notifyInstructionChanged: (Instruction) -> ()
  let preserveDebugInfo: Bool
}

//===----------------------------------------------------------------------===//
//                          Builder initialization
//===----------------------------------------------------------------------===//

extension Builder {
  /// Creates a builder which inserts _before_ `insPnt`, using a custom `location`.
  init(before insPnt: Instruction, location: Location, _ context: some MutatingContext) {
    self.init(insertAt: .before(insPnt), location: location,
              context.notifyInstructionChanged, context._bridged.asNotificationHandler())
  }

  /// Creates a builder which inserts _before_ `insPnt`, using the location of `insPnt`.
  init(before insPnt: Instruction, _ context: some MutatingContext) {
    self.init(insertAt: .before(insPnt), location: insPnt.location,
              context.notifyInstructionChanged, context._bridged.asNotificationHandler())
  }

  /// Creates a builder which inserts _after_ `insPnt`, using a custom `location`.
  init(after insPnt: Instruction, location: Location, _ context: some MutatingContext) {
    if let nextInst = insPnt.next {
      self.init(insertAt: .before(nextInst), location: location,
                context.notifyInstructionChanged, context._bridged.asNotificationHandler())
    } else {
      self.init(insertAt: .atEndOf(insPnt.parentBlock), location: location,
                context.notifyInstructionChanged, context._bridged.asNotificationHandler())
    }
  }

  /// Creates a builder which inserts _after_ `insPnt`, using the location of `insPnt`.
  init(after insPnt: Instruction, _ context: some MutatingContext) {
    self.init(after: insPnt, location: insPnt.location, context)
  }

  /// Creates a builder which inserts at the end of `block`, using a custom `location`.
  init(atEndOf block: BasicBlock, location: Location, _ context: some MutatingContext) {
    self.init(insertAt: .atEndOf(block), location: location,
              context.notifyInstructionChanged, context._bridged.asNotificationHandler())
  }

  /// Creates a builder which inserts at the begin of `block`, using a custom `location`.
  init(atBeginOf block: BasicBlock, location: Location, _ context: some MutatingContext) {
    let firstInst = block.instructions.first!
    self.init(insertAt: .before(firstInst), location: location,
              context.notifyInstructionChanged, context._bridged.asNotificationHandler())
  }

  /// Creates a builder which inserts at the begin of `block`, using the location of the first
  /// instruction of `block`.
  init(atBeginOf block: BasicBlock, _ context: some MutatingContext) {
    let firstInst = block.instructions.first!
    self.init(insertAt: .before(firstInst), location: firstInst.location,
              context.notifyInstructionChanged, context._bridged.asNotificationHandler())
  }

  init(staticInitializerOf global: GlobalVariable, _ context: some MutatingContext) {
    self.init(insertAt: .staticInitializer(global),
              location: Location.artificialUnreachableLocation,
              { _ in }, context._bridged.asNotificationHandler())
  }
}

//===----------------------------------------------------------------------===//
//                          Modifying the SIL
//===----------------------------------------------------------------------===//

extension Undef {
  static func get(type: Type, _ context: some MutatingContext) -> Undef {
    context._bridged.getSILUndef(type.bridged).value as! Undef
  }
}

extension BasicBlock {
  func addBlockArgument(type: Type, ownership: Ownership, _ context: some MutatingContext) -> BlockArgument {
    context.notifyInstructionsChanged()
    return bridged.addBlockArgument(type.bridged, ownership._bridged).blockArgument
  }
  
  func eraseArgument(at index: Int, _ context: some MutatingContext) {
    context.notifyInstructionsChanged()
    bridged.eraseArgument(index)
  }

  func moveAllInstructions(toBeginOf otherBlock: BasicBlock, _ context: some MutatingContext) {
    context.notifyInstructionsChanged()
    context.notifyBranchesChanged()
    bridged.moveAllInstructionsToBegin(otherBlock.bridged)
  }

  func moveAllInstructions(toEndOf otherBlock: BasicBlock, _ context: some MutatingContext) {
    context.notifyInstructionsChanged()
    context.notifyBranchesChanged()
    bridged.moveAllInstructionsToEnd(otherBlock.bridged)
  }

  func eraseAllArguments(_ context: some MutatingContext) {
    // Arguments are stored in an array. We need to erase in reverse order to avoid quadratic complexity.
    for argIdx in (0 ..< arguments.count).reversed() {
      eraseArgument(at: argIdx, context)
    }
  }

  func moveAllArguments(to otherBlock: BasicBlock, _ context: some MutatingContext) {
    bridged.moveArgumentsTo(otherBlock.bridged)
  }
}

extension AllocRefInstBase {
  func setIsStackAllocatable(_ context: some MutatingContext) {
    context.notifyInstructionsChanged()
    bridged.AllocRefInstBase_setIsStackAllocatable()
    context.notifyInstructionChanged(self)
  }
}

extension UseList {
  func replaceAll(with replacement: Value, _ context: some MutatingContext) {
    for use in self {
      use.set(to: replacement, context)
    }
  }
}

extension Operand {
  func set(to value: Value, _ context: some MutatingContext) {
    instruction.setOperand(at: index, to: value, context)
  }
}

extension Instruction {
  func setOperand(at index : Int, to value: Value, _ context: some MutatingContext) {
    if self is FullApplySite && index == ApplyOperands.calleeOperandIndex {
      context.notifyCallsChanged()
    }
    context.notifyInstructionsChanged()
    bridged.setOperand(index, value.bridged)
    context.notifyInstructionChanged(self)
  }
}

extension BuiltinInst {
  func constantFold(_ context: some MutatingContext) -> Value? {
    context._bridged.constantFoldBuiltin(bridged).value
  }
}

extension RefCountingInst {
  func setAtomicity(isAtomic: Bool, _ context: some MutatingContext) {
    context.notifyInstructionsChanged()
    bridged.RefCountingInst_setIsAtomic(isAtomic)
    context.notifyInstructionChanged(self)
  }
}

extension TermInst {
  func replaceBranchTarget(from fromBlock: BasicBlock, to toBlock: BasicBlock, _ context: some MutatingContext) {
    context.notifyBranchesChanged()
    bridged.TermInst_replaceBranchTarget(fromBlock.bridged, toBlock.bridged)
  }
}

extension Function {
  func set(needStackProtection: Bool, _ context: FunctionPassContext) {
    context.notifyEffectsChanged()
    bridged.setNeedStackProtection(needStackProtection)
  }

  func fixStackNesting(_ context: FunctionPassContext) {
    context._bridged.fixStackNesting(bridged)
  }
}
