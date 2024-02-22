//===--- ExperimentalClosureSpecialization.swift ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===-----------------------------------------------------------------------===//

/// AutoDiff Closure Specialization
/// ----------------------
/// This optimization performs closure specialization tailored for the patterns seen
/// in Swift Autodiff. In principle, the optimization does the same thing as the existing
/// closure specialization pass. However, it is tailored to the patterns of Swift Autodiff.
///
/// The compiler performs reverse-mode differentiation on functions marked with `@differentiable(reverse)`.
/// In doing so, it generates corresponding VJP and Pullback functions, which perform the
/// forward and reverse pass respectively. The Pullbacks are essentially a chain of closures,
/// where the closure-contexts are used as an implicit "tape".
///
/// The code patterns that this optimization targets, look similar to the one below:
/// ``` swift
/// @differentiable(reverse)
/// func foo(_ x: Float) -> Float {
///     return sin(x)
/// }
///
/// //============== Before optimization ==============//
/// sil @pb_foo: $(Float, (Float) -> Float) -> Float {
/// bb0(%0: $Float, %1: $(Float) -> Float):
///     %2 = apply %1(%0): $Float
///     return %2: $Float
/// }
///
/// sil @vjp_foo: $(Float) -> (Float, (Float) -> Float) {
/// bb0(%0: $Float):
///     %1 = apply @sin(%0): $Float
///     %2 = partial_apply @pb_sin(%0): $(Float) -> Float
///     %pb_foo = partial_apply @pb_foo(%2): $(Float, (Float) -> Float) -> Float
///     return (%1, %pb_foo)
/// }
///
/// //============== After optimization ==============//
/// sil @specialized_pb_foo: $(Float, Float) -> Float {
/// bb0(%0: $Float, %1: $Float):
///     %2 = partial_apply @pb_sin(%1): $(Float) -> Float
///     %3 = apply %2(%0): $Float
///     return %3: $Float
/// }
///
/// sil @vjp_foo: $(Float) -> (Float, (Float) -> Float) {
/// bb0(%0: $Float):
///     %1 = apply @sin(%0): $Float
///     %pb_foo = partial_apply @specialized_pb_foo(%0): $(Float, Float) -> Float
///     return (%1, %pb_foo)
/// }
/// ```

import SIL

private let verbose = false

private func log(_ message: @autoclosure () -> String) {
  if verbose {
    print("### \(message())")
  }
}

// =========== Entry point =========== //
let autodiffClosureSpecialization = FunctionPass(name: "autodiff-closure-specialize") {
  (function: Function, context: FunctionPassContext) in

  // TODO: Don't optimize functions that are marked with the opt.never
  // attribute.

  // TODO: If F is an external declaration, there is nothing to specialize.

  // If this is not an Autodiff VJP for a differentiable function, exit early.
  if !function.isAutodiffVJP {
    return
  }

  // TODO: Eliminate dead closures, return.

  // TODO: Invalidate everything since we delete calls as well as add new
  // calls and branches.
}

// =========== AutodiffClosureSpecializer ========== //

let specializationLevelLimit = 2

struct AutodiffClosureSpecializer {
  let context: FunctionPassContext

  init(_ context: FunctionPassContext) {
    self.context = context
  }

  func gatherCallSites(in caller: Function) -> Stack<CallSiteDescriptor> {
    // We should not look at reabstraction closures twice who we
    // ultimately ended up using as an argument that we specialize on.
    var usedReabstractionClosures = InstructionSet(self.context)
    defer {
      usedReabstractionClosures.deinitialize()
    }

    var callSiteMap = CallSiteMap()

    for inst in caller.instructions {
      if inst.isSupportedClosure {
        if !usedReabstractionClosures.contains(inst) {
          let closure = inst as! SingleValueInstruction
          let shouldExitEarly = updateCallSites(
            self.context,
            for: closure,
            callSiteMap: &callSiteMap,
            usedReabstractionClosures: &usedReabstractionClosures
          )
          if shouldExitEarly {
            return callSiteMap.intoCallSites(self.context)
          }
        }
      }
    }

    return callSiteMap.intoCallSites(self.context)
  }
}

// ===================== Utility types, extensions and functions ===================== //

private struct OrderedDict<Key: Hashable, Value> {
  private var valueIndexDict: [Key: Int] = [:]
  private var entryList: [(Key, Value)] = []

  public subscript(key: Key) -> Value? {
    if let index = valueIndexDict[key] {
      return entryList[index].1
    }
    return nil
  }

  public mutating func insert(key: Key, value: Value) {
    if valueIndexDict[key] == nil {
      valueIndexDict[key] = entryList.count
      entryList.append((key, value))
    }
  }

  public mutating func update(key: Key, value: Value) {
    if let index = valueIndexDict[key] {
      entryList[index].1 = value
    }
  }

  public var entries: [(Key, Value)] {
    entryList
  }

  public var keys: [Key] {
    entryList.map { $0.0 }
  }

  public var values: [Value] {
    entryList.map { $0.1 }
  }
}

private typealias CallSiteMap = OrderedDict<PartialApplyInst, CallSiteDescriptor>

extension CallSiteMap {
  fileprivate func intoCallSites(_ context: FunctionPassContext) -> Stack<CallSiteDescriptor> {
    var callSites = Stack<CallSiteDescriptor>(context)
    callSites.append(contentsOf: self.values)
    return callSites
  }
}

extension PartialApplyInst {
  /// True, if the closure obtained from this partial_apply is the
  /// pullback returned from this autodiff VJP
  fileprivate var isPullbackInResultOfAutodiffVJP: Bool {
    if !self.parentFunction.isAutodiffVJP {
      return false
    }

    if let use = self.result.firstUse {
      if let tupleInst = use.instruction as? TupleInst {
        let maybeReturnedTuple = tupleInst.result
        let vjp = self.parentFunction
        if let returnInst = vjp.returnInstruction {
          if maybeReturnedTuple == returnInst.returnedValue {
            return true
          }
        }
      }
    }

    return false
  }
}

extension Instruction {
  fileprivate var isSupportedClosure: Bool {
    if !(self is PartialApplyInst || self is ThinToThickFunctionInst) {
      return false
    }

    let callee =
      if self is PartialApplyInst {
        (self as! PartialApplyInst).callee
      } else {
        (self as! ThinToThickFunctionInst).callee
      }

    if callee is FunctionRefInst {
      if let pai = self as? PartialApplyInst {
        return pai.argumentOperands
          .filter { !$0.value.type.isObject }
          .allSatisfy { operand in
            if let convention = pai.convention(of: operand) {
              return convention == .indirectInout || convention == .indirectInoutAliasable
            }
            return false
          }
      }
    } else {
      return false
    }

    return true
  }
}

/// Updates the call site map for the given closure.
///
/// - Parameters:
///   - context: `FunctionPassContext` instance
///   - closure: The closure for which the call sites are being updated.
///   - callSiteMap: `CallSiteMap` instance, shared across all closures.
///   - usedReabstractionClosures: Set of closures that are reabstracted into a final closure,
///     passed in at a callsite.
/// - Returns: `true` if the optimization should exit early, `false` otherwise.
private func updateCallSites(
  _ context: FunctionPassContext,
  for closure: SingleValueInstruction,
  callSiteMap: inout CallSiteMap,
  usedReabstractionClosures: inout InstructionSet
) -> Bool {
  var intermediateClosureArgDescriptors: [IntermediateClosureArgDescriptor] = []

  var (
    remainingUses,
    lifeRangeEndpoints,
    possibleMarkDependenceBases,
    haveUsedReabstraction
  ) = handleNonApplyUses(context, for: closure)

  defer {
    possibleMarkDependenceBases.deinitialize()
  }

  for use in remainingUses {
    lifeRangeEndpoints.append(use.instruction)

    // If the use is not a partial_apply or if it is, but has
    // substitutions or cannot be optimized, then we can't specialize
    // it.
    guard let pai = use.instruction as? PartialApplyInst else {
      continue
    }

    if pai.hasSubstitutions || !pai.canOptimize {
      continue
    }

    if !pai.isPullbackInResultOfAutodiffVJP {
      continue
    }

    // If partial apply-site does not have a function_ref definition as
    // its callee, we cannot do anything here, so continue.
    guard let callee = pai.referencedFunction else {
      continue
    }

    if callee.isAvailableExternally {
      continue
    }

    // Don't specialize non-fragile (read as non-serialized) callees if
    // the caller is fragile; the specialized callee will have shared linkage,
    // and thus cannot be referenced from the fragile caller.
    let caller = closure.parentFunction
    if caller.isSerialized && !callee.isSerialized {
      continue
    }

    // If the callee uses a dynamic Self, we cannot specialize it,
    // since the resulting specialization might longer have 'self'
    // as the last parameter.
    //
    // TODO: We could fix this by inserting new arguments more carefully, or
    // changing how we model dynamic Self altogether.
    if callee.mayBindDynamicSelf {
      finalizeCallSites(
        context,
        for: closure,
        callSiteMap: &callSiteMap,
        lifeRangeEndpoints: lifeRangeEndpoints,
        intermediateClosureArgDescs: intermediateClosureArgDescriptors
      )
      return true
    }

    // Proceed if the closure is passed as an argument (and not called).
    // If it is called we have nothing to do.
    //
    // `closureArgumentIndex` is the index of the closure in the callee's
    // argument list.
    guard let closureArgumentIndex = pai.calleeArgumentIndex(of: use) else {
      continue
    }

    // Ok, we know that we can perform the optimization but not whether or
    // not the optimization is profitable. Check if the closure is actually
    // called in the callee (or in a function called by the callee).
    if !isClosureApplied(in: callee, closureArgIndex: closureArgumentIndex) {
      continue
    }

    // Gather the parameter info for the closure argument, at the apply site.
    guard let closureParamInfo = pai.operandConventions[parameter: use.index] else {
      fatalError("Parameter info not found for operand: \(use)!")
    }

    // We currently only support copying intermediate reabstraction
    // closures if the closure is ultimately passed trivially.
    let calleeConv = FunctionConvention(for: closureParamInfo.type, in: callee)
    let isClosurePassedTrivially = calleeConv.isTrivialNoescape

    // Mark the reabstraction closures as used.
    if haveUsedReabstraction {
      markReabstractionPartialApplyAsUsed(
        firstClosure: closure, currentClosure: use.value,
        usedReabstractionClosures: &usedReabstractionClosures
      )

      if !isClosurePassedTrivially {
        continue
      }
    }

    // Get all non-failure exit BBs in the Apply Callee if our partial apply
    // is guaranteed. If we do not understand one of the exit BBs, bail.
    //
    // We need this to make sure that we insert a release in the appropriate
    // locations to balance the +1 from the creation of the partial apply.
    //
    // However, thin_to_thick_function closures don't have a context and
    // don't need to be released.
    guard
      case Function.NonFailureExitBBsSearchResult.allBlocksUnderstood(let nonFailureExitBBs) =
        callee.findAllNonFailureExitBBs()
    else {
      continue
    }

    let onlyHaveThinToThickClosure =
      closure is ThinToThickFunctionInst && !haveUsedReabstraction

    if (closureParamInfo.convention.isGuaranteed || isClosurePassedTrivially)
      && !onlyHaveThinToThickClosure
    {
      continue
    }

    // Specializing a readnone, readonly, releasenone function with a
    // nontrivial context is illegal. Inserting a release in such a function
    // results in miscompilation after other optimizations.
    // For now, the specialization is disabled.
    //
    // TODO: A @noescape closure should never be converted to an @owned
    // argument regardless of the function attribute.
    if !onlyHaveThinToThickClosure && callee.hasIllegalEffectForSpecialization {
      continue
    }

    // Avoid an infinite specialization loop caused by repeated runs of
    // ClosureSpecializer and CapturePropagation.
    // CapturePropagation propagates constant function-literals. Such
    // function specializations can then be optimized again by the
    // ClosureSpecializer and so on.
    // This happens if a closure argument is called _and_ referenced in
    // another closure, which is passed to a recursive call. E.g.
    //
    // func foo(_ c: @escaping () -> ()) {
    //   c()
    //   foo({ c() })
    // }
    //
    // A limit of 2 is good enough and will not be exceed in "regular"
    // optimization scenarios.
    let closureCallee =
      closure is PartialApplyInst
      ? (closure as! PartialApplyInst).referencedFunction!
      : (closure as! ThinToThickFunctionInst).referencedFunction!

    if closureCallee.specializationLevel > specializationLevelLimit {
      continue
    }

    if callSiteMap[pai] == nil {
      callSiteMap.insert(key: pai, value: CallSiteDescriptor(applySite: pai))
    }

    intermediateClosureArgDescriptors.append(
      IntermediateClosureArgDescriptor(
        callSite: pai,
        closureArgumentIndex: closureArgumentIndex,
        parameterInfo: closureParamInfo,
        nonFailureExitBBs: nonFailureExitBBs
      )
    )
  }

  finalizeCallSites(
    context,
    for: closure,
    callSiteMap: &callSiteMap,
    lifeRangeEndpoints: lifeRangeEndpoints,
    intermediateClosureArgDescs: intermediateClosureArgDescriptors
  )

  return false
}

/// Handles conversion and reabstraction uses of the supported closure.
///
/// - Parameters:
///   - context: `FunctionPassContext` instance
///   - for: A closure against which callees may be specialized.
/// - Returns:
///   - remainingUses: Any uses not handled in this function.
///   - lifeRangeEndpoints: Instructions that comprise the possible life-range of the closure.
///   - possibleMarkDependenceBases: The possible mark_dependence bases of the closure.
///   - haveUsedReabstraction: Whether the closure has gone throug a reabstraction.
private func handleNonApplyUses(
  _ context: FunctionPassContext,
  for closure: SingleValueInstruction
) -> (
  remainingUses: [Operand], lifeRangeEndpoints: [Instruction],
  possibleMarkDependenceBases: ValueSet, haveUsedReabstraction: Bool
) {
  var initialUses = Array(closure.uses)
  var remainingUses = [Operand]()
  var lifeRangeEndpoints: [Instruction] = []
  var haveUsedReabstraction = false

  // Set of possible arguments for mark_dependence. The base of a
  // mark_dependence we copy must be available in the specialized function.
  var possibleMarkDependenceBases = ValueSet(context)

  if let pai = closure as? PartialApplyInst {
    for arg in pai.arguments {
      possibleMarkDependenceBases.insert(arg)
    }
  }

  // Uses may grow in this loop to handle transitive uses
  for useIndex in 0..<initialUses.count {
    let use = initialUses[useIndex]
    lifeRangeEndpoints.append(use.instruction)

    // Recurse through conversions
    if let cfi = use.instruction as? ConvertFunctionInst {
      initialUses.append(contentsOf: cfi.uses)
      possibleMarkDependenceBases.insert(cfi)
      continue
    }

    if let cvt = use.instruction as? ConvertEscapeToNoEscapeInst {
      initialUses.append(contentsOf: cvt.uses)
      possibleMarkDependenceBases.insert(cvt)
      continue
    }

    // Look through reabstraction thunks
    if let pai = use.instruction as? PartialApplyInst {
      // Only look at reabstraction thunks if the closure obtained
      // as a result of this partial_apply is not the pullback returned
      // from this autodiff VJP.
      if !pai.isPullbackInResultOfAutodiffVJP {
        // Reabstraction can cause series of partial_apply to be emitted. It
        // is okay to treat these like conversion instructions. Current
        // restriction: if the partial_apply does not take ownership of its
        // argument we don't need to analyze which partial_apply to emit
        // release for (its all of them).
        let calleeType = pai.callee.type
        if pai.isPartialApplyOfReabstractionThunk
          && pai.isSupportedClosure
          && calleeType.isNoEscapeFunction
          && calleeType.isThickFunction
        {
          initialUses.append(contentsOf: pai.uses)
          possibleMarkDependenceBases.insert(pai)
          haveUsedReabstraction = true
        }
        continue
      }
    }

    // Look through mark_dependence on partial_apply[stack]
    if let mdi = use.instruction as? MarkDependenceInst {
      // We can't copy a closure if the mark_dependence base is not
      // available in the specialized function.
      if !possibleMarkDependenceBases.contains(mdi.base) {
        continue
      }
      if mdi.value == use.value
        && mdi.value.type.isNoEscapeFunction
        && mdi.value.type.isThickFunction
      {
        initialUses.append(contentsOf: mdi.uses)
        continue
      }
    }

    remainingUses.append(use)
  }

  return (remainingUses, lifeRangeEndpoints, possibleMarkDependenceBases, haveUsedReabstraction)
}

/// Finalizes the callsites for a closure by adding a corresponding `ClosureArgDescriptor`
/// to all call sites where the closure is passed as an argument.
///
/// - Parameters:
///   - context: `FunctionPassContext` instance
///   - closure: The closure for which the call sites are being updated.
///   - callSiteMap: `CallSiteMap` instance, shared across all closures.
///   - lifeRangeEndpoints: Instructions that comprise the possible life-range of the closure.
///   - intermediateClosureArgDescs: Intermediate closure argument descriptors.
private func finalizeCallSites(
  _ context: FunctionPassContext,
  for closure: SingleValueInstruction,
  callSiteMap: inout CallSiteMap,
  lifeRangeEndpoints: [Instruction],
  intermediateClosureArgDescs: [IntermediateClosureArgDescriptor]
) {
  var lifeRange = InstructionRange(begin: closure, context)
  defer {
    lifeRange.deinitialize()
  }
  lifeRange.insert(contentsOf: lifeRangeEndpoints)

  let closureInfo = ClosureInfo(closure: closure, lifetimeFrontier: Array(lifeRange.ends))

  intermediateClosureArgDescs
    .reduce(into: [:]) { result, imClosureArgDesc in
      result[imClosureArgDesc.callSite, default: []].append(
        imClosureArgDesc.intoClosureArgDescriptor(closureInfo: closureInfo))
    }
    .forEach { callSite, closureArgDescriptors in
      if var callSiteDesc = callSiteMap[callSite] {
        callSiteDesc.appendClosureArgDescriptors(closureArgDescriptors)
        callSiteMap.update(key: callSite, value: callSiteDesc)
      } else {
        fatalError("Call site descriptor not found for call site: \(callSite)!")
      }
    }
}

private func isClosureApplied(in callee: Function, closureArgIndex index: Int) -> Bool {
  func inner(_ callee: Function, _ index: Int, _ handledFuncs: inout Set<Function>) -> Bool {
    let closureArg = callee.argument(at: index)

    for use in closureArg.uses {
      if let fai = use.instruction as? FullApplySite {
        if fai.callee == closureArg {
          return true
        }

        if let faiCallee = fai.referencedFunction {
          if faiCallee.isAvailableExternally
            && !handledFuncs.contains(faiCallee)
            && handledFuncs.count < recursionBudget
          {
            handledFuncs.insert(faiCallee)
            if inner(faiCallee, fai.calleeArgumentIndex(of: use)!, &handledFuncs) {
              return true
            }
          }
        }
      }
    }

    return false
  }

  // Limit the number of recursive calls to not go into
  // exponential behavior in corner cases.
  let recursionBudget = 8
  var handledFuncs: Set<Function> = []
  return inner(callee, index, &handledFuncs)
}

/// Marks any intermediate, reabstracted closures, corresponding to given closure
/// passed in at a callsite, as used. We do not want to look at reabstraction closures
/// twice as they are covered as part of the final closure that we're passing in at the
/// callsite.
///
/// - Parameters:
///   - firstClosure: The original "supported" closure instance in the function.
///   - currentClosure: The current closure that one or more "reabstractions" removed from the
///     `firstClosure` and should be marked as used.
///   - usedReabstractionClosures: Set of closures that are reabstracted into a final closure,
///     passed in at a callsite.
private func markReabstractionPartialApplyAsUsed(
  firstClosure: Value,
  currentClosure: Value,
  usedReabstractionClosures: inout InstructionSet
) {
  if currentClosure == firstClosure {
    return
  }

  if let pai = currentClosure as? PartialApplyInst {
    usedReabstractionClosures.insert(pai)
    return markReabstractionPartialApplyAsUsed(
      firstClosure: firstClosure, currentClosure: pai.callee,
      usedReabstractionClosures: &usedReabstractionClosures)
  }

  if let cvt = currentClosure as? ConvertFunctionInst {
    usedReabstractionClosures.insert(cvt)
    return markReabstractionPartialApplyAsUsed(
      firstClosure: firstClosure, currentClosure: cvt.fromFunction,
      usedReabstractionClosures: &usedReabstractionClosures)
  }

  if let cvt = currentClosure as? ConvertEscapeToNoEscapeInst {
    usedReabstractionClosures.insert(cvt)
    return markReabstractionPartialApplyAsUsed(
      firstClosure: firstClosure, currentClosure: cvt.fromFunction,
      usedReabstractionClosures: &usedReabstractionClosures)
  }

  if let mdi = currentClosure as? MarkDependenceInst {
    usedReabstractionClosures.insert(mdi)
    return markReabstractionPartialApplyAsUsed(
      firstClosure: firstClosure, currentClosure: mdi.value,
      usedReabstractionClosures: &usedReabstractionClosures)
  }

  fatalError("Unexpected instruction used during closure reabstraction: \(currentClosure)")
}

// ===================== Types ===================== //

/// Represents all the information required to
/// represent a closure in isolation, i.e., outside of
/// a callsite context where the closure may be getting
/// passed as an argument.
///
/// Composed with other information inside a `ClosureArgDescriptor`
/// to represent a closure as an argument at a callsite.
struct ClosureInfo {
  let closure: SingleValueInstruction
  let lifetimeFrontier: [Instruction]

  init(closure: SingleValueInstruction, lifetimeFrontier: [Instruction]) {
    self.closure = closure
    self.lifetimeFrontier = lifetimeFrontier
  }

}

/// Represents a closure as an argument at a callsite.
struct ClosureArgDescriptor {
  let closureInfo: ClosureInfo
  /// The index of the closure in the callsite's argument list.
  let closureArgumentIndex: Int
  let parameterInfo: ParameterInfo
  /// This is only needed if we have guaranteed parameters. In most cases
  /// it will have only one element, a return inst.
  var nonFailureExitBBs: [BasicBlock]
}

/// An intermediate data structure that holds the information about
/// a closure at a callsite that will be specialized on it.
///
/// This type is an implementation detail and exists only to delay the creation
/// of the final `ClosureArgDescriptor`s until the complete lifetime frontier of
/// the corresponding closure has been computed.
private struct IntermediateClosureArgDescriptor {
  let callSite: PartialApplyInst
  let closureArgumentIndex: Int
  let parameterInfo: ParameterInfo
  let nonFailureExitBBs: [BasicBlock]

  public func intoClosureArgDescriptor(
    closureInfo: ClosureInfo
  ) -> ClosureArgDescriptor {
    return ClosureArgDescriptor(
      closureInfo: closureInfo,
      closureArgumentIndex: closureArgumentIndex,
      parameterInfo: parameterInfo,
      nonFailureExitBBs: nonFailureExitBBs
    )
  }
}

/// Represents a callsite containing one or more closure arguments.
struct CallSiteDescriptor {
  let applySite: ApplySite
  var closureArgDescriptors: [ClosureArgDescriptor] = []
  var silArgIndexToClosureArgDescIndex: [Int: Int] = [:]

  public init(applySite: ApplySite) {
    self.applySite = applySite
  }

  public mutating func appendClosureArgDescriptor(_ descriptor: ClosureArgDescriptor) {
    self.silArgIndexToClosureArgDescIndex[descriptor.closureArgumentIndex] =
      self.closureArgDescriptors.count
    self.closureArgDescriptors.append(descriptor)
  }

  public mutating func appendClosureArgDescriptors(_ descriptors: [ClosureArgDescriptor]) {
    for descriptor in descriptors {
      self.appendClosureArgDescriptor(descriptor)
    }
  }
}

// ===================== Unit tests ===================== //

let gatherCallSitesTest = FunctionTest("closure_specialize_gather_call_sites") {
  function, arguments, context in

  print("Specializing closures in function: \(function.name)")
  print("===============================================")
  let closureSpecializer = AutodiffClosureSpecializer(context)
  var callSites = closureSpecializer.gatherCallSites(in: function)
  defer {
    callSites.deinitialize()
  }

  callSites.forEach { callSite in
    print("PartialApply call site: \(callSite.applySite)")
    print("Passed in closures: ")
    for index in callSite.closureArgDescriptors.indices {
      var closureArgDescriptor = callSite.closureArgDescriptors[index]
      print("\(index+1). \(closureArgDescriptor.closureInfo.closure)")
    }
  }
  print("\n")
}
