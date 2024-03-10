//===--- AutodiffClosureSpecialization.swift ---------------------------===//
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
/// This optimization performs closure specialization tailored for the patterns seen in Swift Autodiff. In principle,
/// the optimization does the same thing as the existing closure specialization pass. However, it is tailored to the
/// patterns of Swift Autodiff.
///
/// The compiler performs reverse-mode differentiation on functions marked with `@differentiable(reverse)`. In doing so,
/// it generates corresponding VJP and Pullback functions, which perform the forward and reverse pass respectively. You
/// can think of VJPs as functions that "differentiate" an original function and Pullbacks as the calculated
/// "derivative" of the original function. 
/// 
/// VJPs always return a tuple of 2 values -- the original result and the Pullback. Pullbacks are essentially a chain 
/// of closures, where the closure-contexts are implicitly used as the so-called "tape" during the reverse
/// differentiation process. It is this chain of closures contained within the Pullbacks that this optimization aims
/// to optimize via closure specialization.
///
/// The code patterns that this optimization targets, look similar to the one below:
/// ``` swift
/// 
/// // Since `foo` is marked with the `differentiable(reverse)` attribute the compiler
/// // will generate corresponding VJP and Pullback functions in SIL. Let's assume that
/// // these functions are called `vjp_foo` and `pb_foo` respectively.
/// @differentiable(reverse) 
/// func foo(_ x: Float) -> Float { 
///   return sin(x)
/// }
///
/// //============== Before optimization ==============// 
/// // VJP of `foo`. Returns the original result and the Pullback of `foo`.
/// sil @vjp_foo: $(Float) -> (originalResult: Float, pullback: (Float) -> Float) { 
/// bb0(%0: $Float): 
///   // vjp_sin returns the original result `sin(x)`, and the pullback of sin, `pb_sin`.
///   // `pb_sin` is a closure.
///   %(originalResult, pb_sin) = apply @vjp_sin(%0): $(Float) -> (Float, (Float) -> Float)
///  
///   %pb_foo = function_ref @pb_foo: $@convention(thin) (Float, (Float) -> Float) -> Float
///   %partially_applied_pb_foo = partial_apply %pb_foo(%pb_sin): $(Float, (Float) -> Float) -> Float
///  
///   return (%originalResult, %partially_applied_pb_foo)
/// }
///
/// // Pullback of `foo`. 
/// //
/// // It receives what are called as intermediate closures that represent
/// // the calculations that the Pullback needs to perform to calculate a function's
/// // derivative.
/// //
/// // The intermediate closures may themselves contain intermediate closures and
/// // that is why the Pullback for a function differentiated at the "top" level
/// // may end up being a "chain" of closures.
/// sil @pb_foo: $(Float, (Float) -> Float) -> Float { 
/// bb0(%0: $Float, %pb_sin: $(Float) -> Float): 
///   %derivative_of_sin = apply %pb_sin(%0): $Float return %2: $Float
///   return %derivative_of_sin: Float
/// }
///
/// //============== After optimization ==============// 
/// sil @vjp_foo: $(Float) -> (originalResult: Float, pullback: (Float) -> Float) { 
/// bb0(%0: $Float): 
///   %1 = apply @sin(%0): $(Float) -> Float 
/// 
///   // Before the optimization, pullback of `foo` used to take a closure for computing
///   // pullback of `sin`. Now, the specialized pullback of `foo` takes the arguments that
///   // pullback of `sin` used to close over and pullback of `sin` is instead copied over
///   // inside pullback of `foo`.
///   %specialized_pb_foo = function_ref @pb_foo: $@convention(thin) (Float, Float) -> Float
///   %partially_applied_pb_foo = partial_apply @specialized_pb_foo(%0): $(Float, Float) -> Float 
/// 
///   return (%1, %partially_applied_pb_foo)
/// }
/// 
/// sil @specialized_pb_foo: $(Float, Float) -> Float { 
/// bb0(%0: $Float, %1: $Float): 
///   %2 = partial_apply @pb_sin(%1): $(Float) -> Float 
///   %3 = apply %2(%0): $Float 
///   return %3: $Float
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
  if !function.isAutodiffVJP {
    return
  }
}

// =========== Top-level functions ========== //

private let specializationLevelLimit = 2

private func gatherCallSites(in caller: Function, _ context: FunctionPassContext) -> [CallSite] {
  /// __Root__ closures created via `partial_apply` or `thin_to_thick_function` may be converted and reabstracted
  /// before finally being used at an apply site. We do not want to handle these intermediate closures separately
  /// as they are handled and cloned into the specialized function as part of the root closures. Therefore, we keep 
  /// track of these intermediate closures in a set. 
  /// 
  /// This set is populated via the `markConvertedAndReabstractedClosuresAsUsed` function which is called when we're
  /// handling the different uses of our root closures.
  ///
  /// Below SIL example illustrates the above point.
  /// ```                                                                                                      
  /// // The below set of a "root" closure and its reabstractions/conversions
  /// // will be handled as a unit and the entire set will be copied over
  /// // in the specialized version of `takesClosure` if we determine that we  
  /// // can specialize `takesClosure` against its closure argument.
  ///                                                                                                          __            
  /// %someFunction = function_ref @someFunction: $@convention(thin) (Int, Int) -> Int                            \ 
  /// %rootClosure = partial_apply [callee_guaranteed] %someFunction (%someInt): $(Int, Int) -> Int                \
  /// %thunk = function_ref @reabstractionThunk : $@convention(thin) (@callee_guaranteed (Int) -> Int) -> @out Int /     
  /// %reabstractedClosure = partial_apply [callee_guaranteed] %thunk(%rootClosure) :                             /      
  ///                        $@convention(thin) (@callee_guaranteed (Int) -> Int) -> @out Int                  __/       
  /// 
  /// %takesClosure = function_ref @takesClosure : $@convention(thin) (@owned @callee_guaranteed (Int) -> @out Int) -> Int
  /// %result = partial_apply %takesClosure(%reabstractedClosure) : $@convention(thin) (@owned @callee_guaranteed () -> @out Int) -> Int
  /// ret %result
  /// ```
  var convertedAndReabstractedClosures = InstructionSet(context)

  defer {
    convertedAndReabstractedClosures.deinitialize()
  }

  var callSiteMap = CallSiteMap()

  for inst in caller.instructions {
    if !convertedAndReabstractedClosures.contains(inst),
       let rootClosure = inst.asSupportedClosure
    {
      updateCallSites(for: rootClosure, callSiteMap: &callSiteMap, 
                      convertedAndReabstractedClosures: &convertedAndReabstractedClosures, context)
    }
  }

  return callSiteMap.callSites
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

  public var keys: LazyMapSequence<Array<(Key, Value)>, Key> {
    entryList.lazy.map { $0.0 }
  }

  public var values: LazyMapSequence<Array<(Key, Value)>, Value> {
    entryList.lazy.map { $0.1 }
  }
}

private typealias CallSiteMap = OrderedDict<PartialApplyInst, CallSite>

private extension CallSiteMap {
  var callSites: [CallSite] {
    Array(self.values)
  }
}

private extension PartialApplyInst {
  /// True, if the closure obtained from this partial_apply is the
  /// pullback returned from an autodiff VJP
  var isPullbackInResultOfAutodiffVJP: Bool {
    if self.parentFunction.isAutodiffVJP,
       let use = self.uses.singleUse,
       let tupleInst = use.instruction as? TupleInst,
       let returnInst = self.parentFunction.returnInstruction,
      tupleInst == returnInst.returnedValue
    {
      return true
    }

    return false
  }

  var hasNonInoutIndirectArguments: Bool {
    self.argumentOperands
      .filter { !$0.value.type.isObject }
      .allSatisfy {
        if let convention = self.convention(of: $0) {
          return convention == .indirectInout || convention == .indirectInoutAliasable
        }
        return false
      } 
  }
}

private extension Instruction {
  var asSupportedClosure: SingleValueInstruction? {
    switch self {
    case let tttf as ThinToThickFunctionInst where tttf.callee is FunctionRefInst:
      return tttf
    case let pai as PartialApplyInst where pai.callee is FunctionRefInst && pai.hasNonInoutIndirectArguments:
      return pai
    default:
      return nil
    }
  }

  var isSupportedClosure: Bool {
    asSupportedClosure != nil
  }
}

private extension ApplySite {
  var calleeIsDynamicFunctionRef: Bool {
    return !(callee is DynamicFunctionRefInst || callee is PreviousDynamicFunctionRefInst)
  }
}

private extension Function {
  var effectAllowsSpecialization: Bool {
    switch bridged.getEffectAttribute() {
    case .ReadNone, .ReadOnly, .ReleaseNone: return false
    default: return true
    }
  }
}

private func updateCallSites(for rootClosure: SingleValueInstruction, callSiteMap: inout CallSiteMap, 
                             convertedAndReabstractedClosures: inout InstructionSet, _ context: FunctionPassContext) {
  var allDirectAndTransitiveUses: [Instruction] = []
  
  var conversionsAndReabstractions = ValueWorklist(context)                            
  defer {
    conversionsAndReabstractions.deinitialize()
  }

  var remainingUses = ValueWorklist(context)                            
  defer {
    remainingUses.deinitialize()
  }

  // A "root" closure undergoing conversions and/or reabstractions has additional restrictions placed upon it, inorder
  // for a call site to be specialized against it. We handle conversion/reabstraction uses before we handle apply uses
  // to gather the parameters required to evaluate these restrictions or to skip call site uses of "unsupported" 
  // closures altogether.
  //
  // There are currently 2 restrictions that are evaluated prior to specializing a callsite against a converted and/or 
  // reabstracted closure -
  // 1. A reabstracted root closure can only be specialized against, if the reabstracted closure is ultimately passed
  //    trivially (as a noescape+thick function) into the call site.
  //
  // 2. A root closure may be a partial_apply [stack], in which case we need to make sure that all mark_dependence 
  //    bases for it will be available in the specialized callee in case the call site is specialized against this root
  //    closure.

  let haveUsedReabstraction = 
    handleConversionsAndReabstractions(for: rootClosure, 
                                       conversionsAndReabstractions: &conversionsAndReabstractions,
                                       remainingUses: &remainingUses,
                                       allDirectAndTransitiveUses: &allDirectAndTransitiveUses, context);


  let intermediateClosureArgDescriptors: [IntermediateClosureArgDescriptor] = 
    handleApplies(for: rootClosure, callSiteMap: &callSiteMap, remainingUses: &remainingUses, 
                  allDirectAndTransitiveUses: &allDirectAndTransitiveUses, 
                  convertedAndReabstractedClosures: &convertedAndReabstractedClosures,
                  haveUsedReabstraction: haveUsedReabstraction, context)

  finalizeCallSites(for: rootClosure, callSiteMap: &callSiteMap, allDirectAndTransitiveUses: allDirectAndTransitiveUses,
                    intermediateClosureArgDescs: intermediateClosureArgDescriptors, context)
}

private func handleApplies(for rootClosure: SingleValueInstruction, callSiteMap: inout CallSiteMap, 
                           remainingUses: inout ValueWorklist, allDirectAndTransitiveUses: inout [Instruction], 
                           convertedAndReabstractedClosures: inout InstructionSet, haveUsedReabstraction: Bool, 
                           _ context: FunctionPassContext) -> [IntermediateClosureArgDescriptor] 
{
  var intermediateClosureArgDescriptors: [IntermediateClosureArgDescriptor] = []
  
  while let use = remainingUses.pop() {
    for use in use.uses {
      allDirectAndTransitiveUses.append(use.instruction)

      guard let pai = use.instruction as? PartialApplyInst else {
        continue
      }

      if pai.hasSubstitutions || !pai.calleeIsDynamicFunctionRef || !pai.isPullbackInResultOfAutodiffVJP {
        continue
      }

      guard let callee = pai.referencedFunction else {
        continue
      }

      if callee.isAvailableExternally {
        continue
      }

      // Don't specialize non-fragile (read as non-serialized) callees if the caller is fragile; the specialized callee
      // will have shared linkage, and thus cannot be referenced from the fragile caller.
      let caller = rootClosure.parentFunction
      if caller.isSerialized && !callee.isSerialized {
        continue
      }

      // If the callee uses a dynamic Self, we cannot specialize it, since the resulting specialization might longer have
      // 'self' as the last parameter.
      //
      // TODO: We could fix this by inserting new arguments more carefully, or changing how we model dynamic Self
      // altogether.
      if callee.mayBindDynamicSelf {
        continue
      }

      // Proceed if the closure is passed as an argument (and not called). If it is called we have nothing to do.
      //
      // `closureArgumentIndex` is the index of the closure in the callee's argument list.
      guard let closureArgumentIndex = pai.calleeArgumentIndex(of: use) else {
        continue
      }

      // Ok, we know that we can perform the optimization but not whether or not the optimization is profitable. Check if
      // the closure is actually called in the callee (or in a function called by the callee).
      if !isClosureApplied(in: callee, closureArgIndex: closureArgumentIndex) {
        continue
      }

      // We currently only support copying intermediate reabstraction closures if the final closure is ultimately passed
      // trivially.
      let closureType = use.value.type
      let isClosurePassedTrivially = closureType.isNoEscapeFunction && closureType.isThickFunction

      // Mark the converted/reabstracted closures as used.
      if haveUsedReabstraction {
        markConvertedAndReabstractedClosuresAsUsed(rootClosure: rootClosure, convertedAndReabstractedClosure: use.value, 
                                                  convertedAndReabstractedClosures: &convertedAndReabstractedClosures)

        if !isClosurePassedTrivially {
          continue
        }
      }

      let onlyHaveThinToThickClosure = rootClosure is ThinToThickFunctionInst && !haveUsedReabstraction

      guard let closureParamInfo = pai.operandConventions[parameter: use.index] else {
        fatalError("Parameter info not found for operand: \(use)!")
      }

      if (closureParamInfo.convention.isGuaranteed || isClosurePassedTrivially)
        && !onlyHaveThinToThickClosure
      {
        continue
      }

      // Functions with a readnone, readonly or releasenone effect and a nontrivial context cannot be specialized.
      // Inserting a release in such a function results in miscompilation after other optimizations. For now, the
      // specialization is disabled.
      //
      // TODO: A @noescape closure should never be converted to an @owned argument regardless of the function's effect
      // attribute.
      if !callee.effectAllowsSpecialization && !onlyHaveThinToThickClosure {
        continue
      }

      // Avoid an infinite specialization loop caused by repeated runs of ClosureSpecializer and CapturePropagation.
      // CapturePropagation propagates constant function-literals. Such function specializations can then be optimized
      // again by the ClosureSpecializer and so on. This happens if a closure argument is called _and_ referenced in
      // another closure, which is passed to a recursive call. E.g.
      //
      // func foo(_ c: @escaping () -> ()) { 
      //  c() foo({ c() })
      // }
      //
      // A limit of 2 is good enough and will not be exceed in "regular" optimization scenarios.
      let closureCallee = rootClosure is PartialApplyInst 
                          ? (rootClosure as! PartialApplyInst).referencedFunction!
                          : (rootClosure as! ThinToThickFunctionInst).referencedFunction!

      if closureCallee.specializationLevel > specializationLevelLimit {
        continue
      }

      if callSiteMap[pai] == nil {
        callSiteMap.insert(key: pai, value: CallSite(applySite: pai))
      }

      intermediateClosureArgDescriptors.append(
        IntermediateClosureArgDescriptor(callSite: pai, closureArgumentIndex: closureArgumentIndex, 
                                        parameterInfo: closureParamInfo, 
                                        reachableExitBBs: Array(callee.blocks.filter { $0.isReachableExitBlock })
      ))
    } 
  }
  return intermediateClosureArgDescriptors
}

private func handleConversionsAndReabstractions(for rootClosure: SingleValueInstruction,
                                                conversionsAndReabstractions: inout ValueWorklist, 
                                                remainingUses: inout ValueWorklist,
                                                allDirectAndTransitiveUses: inout [Instruction], 
                                                _ context: FunctionPassContext) -> Bool 
{
  var haveUsedReabstraction = false

  /// The root closure may be a `partial_apply [stack]` and we need to make sure that all `mark_dependence` bases for 
  /// it will be available in the specialized callee, in case the call site is specialized against this root closure.
  /// 
  /// `possibleMarkDependenceBases` keeps track of all potential values that may be used as bases for creating
  /// `mark_dependence`s for our `onStack` root closure. In the obvious cases these values might be non-trivial closure
  /// captures (which are always available as function arguments in the specialized callee), but they may also be 
  /// conversions/reabstractions of the root closure.
  /// 
  /// Any value outside of the aforementioned values is not going to be available in the specialized callee and a 
  /// `mark_dependence` of the root closure on such a value means that we cannot specialize the call site against it.
  var possibleMarkDependenceBases = ValueSet(context)
  defer {
    possibleMarkDependenceBases.deinitialize()
  }

  if let pai = rootClosure as? PartialApplyInst {
    for arg in pai.arguments {
      possibleMarkDependenceBases.insert(arg)
    }
  }

  conversionsAndReabstractions.pushIfNotVisited(rootClosure)
  
  while let currUse = conversionsAndReabstractions.pop() {
    for use in currUse.uses {
      allDirectAndTransitiveUses.append(use.instruction)

      // Recurse through conversions
      if let cfi = use.instruction as? ConvertFunctionInst {
        conversionsAndReabstractions.pushIfNotVisited(cfi)
        possibleMarkDependenceBases.insert(cfi)
        continue
      }

      if let cvt = use.instruction as? ConvertEscapeToNoEscapeInst {
        conversionsAndReabstractions.pushIfNotVisited(cvt)
        possibleMarkDependenceBases.insert(cvt)
        continue
      }

      // Look through reabstraction thunks
      if let pai = use.instruction as? PartialApplyInst,
         !pai.isPullbackInResultOfAutodiffVJP,
         pai.isPartialApplyOfReabstractionThunk,
         pai.isSupportedClosure,
         pai.callee.type.isNoEscapeFunction,
         pai.callee.type.isThickFunction
      {
        conversionsAndReabstractions.pushIfNotVisited(pai)
        possibleMarkDependenceBases.insert(pai)
        haveUsedReabstraction = true
        continue
      }

      // Look through `mark_dependence` on partial_apply[stack]
      if let mdi = use.instruction as? MarkDependenceInst,
         possibleMarkDependenceBases.contains(mdi.base),  
         mdi.value == use.value,
         mdi.value.type.isNoEscapeFunction,
         mdi.value.type.isThickFunction
      {
        conversionsAndReabstractions.pushIfNotVisited(mdi)
        continue
      }

      // Only add values used in `partial_apply`s as remaining uses
      if use.instruction is PartialApplyInst {
        remainingUses.pushIfNotVisited(currUse)
        allDirectAndTransitiveUses.removeLast()
      }
    }
  }

  return haveUsedReabstraction
}

/// Finalizes the call sites for a given root closure by adding a corresponding `ClosureArgDescriptor`
/// to all call sites where the closure is ultimately passed as an argument.
private func finalizeCallSites(for rootClosure: SingleValueInstruction, callSiteMap: inout CallSiteMap, 
                               allDirectAndTransitiveUses: [Instruction], 
                               intermediateClosureArgDescs: [IntermediateClosureArgDescriptor], 
                               _ context: FunctionPassContext) 
{
  var lifeRange = InstructionRange(begin: rootClosure, context)
  defer {
    lifeRange.deinitialize()
  }
  lifeRange.insert(contentsOf: allDirectAndTransitiveUses)

  let closureInfo = ClosureInfo(closure: rootClosure, lifetimeFrontier: Array(lifeRange.ends))

  intermediateClosureArgDescs
    .reduce(into: [:]) { result, imClosureArgDesc in
      result[imClosureArgDesc.callSite, default: []]
        .append(imClosureArgDesc.intoClosureArgDescriptor(closureInfo: closureInfo))
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

        if let faiCallee = fai.referencedFunction,
           faiCallee.isAvailableExternally,
           !handledFuncs.contains(faiCallee),
           handledFuncs.count < recursionBudget
        {
          handledFuncs.insert(faiCallee)
          if inner(faiCallee, fai.calleeArgumentIndex(of: use)!, &handledFuncs) {
            return true
          }
        }
      }
    }

    return false
  }

  // Limit the number of recursive calls to not go into exponential behavior in corner cases.
  let recursionBudget = 8
  var handledFuncs: Set<Function> = []
  return inner(callee, index, &handledFuncs)
}

/// Marks any converted/reabstracted closures, corresponding to a given root closure as used. We do not want to 
/// look at such closures separately as during function specialization they will be handled as part of the root closure. 
private func markConvertedAndReabstractedClosuresAsUsed(rootClosure: Value, convertedAndReabstractedClosure: Value, 
                                                        convertedAndReabstractedClosures: inout InstructionSet) 
{
  if convertedAndReabstractedClosure == rootClosure {
    return
  }

  if let pai = convertedAndReabstractedClosure as? PartialApplyInst {
    convertedAndReabstractedClosures.insert(pai)
    return 
      markConvertedAndReabstractedClosuresAsUsed(rootClosure: rootClosure, convertedAndReabstractedClosure: pai.callee, 
                                                 convertedAndReabstractedClosures: &convertedAndReabstractedClosures)
  }

  if let cvt = convertedAndReabstractedClosure as? ConvertFunctionInst {
    convertedAndReabstractedClosures.insert(cvt)
    return 
      markConvertedAndReabstractedClosuresAsUsed(rootClosure: rootClosure, 
                                                 convertedAndReabstractedClosure: cvt.fromFunction,
                                                 convertedAndReabstractedClosures: &convertedAndReabstractedClosures)
  }

  if let cvt = convertedAndReabstractedClosure as? ConvertEscapeToNoEscapeInst {
    convertedAndReabstractedClosures.insert(cvt)
    return 
      markConvertedAndReabstractedClosuresAsUsed(rootClosure: rootClosure, 
                                                 convertedAndReabstractedClosure: cvt.fromFunction,
                                                 convertedAndReabstractedClosures: &convertedAndReabstractedClosures)
  }

  if let mdi = convertedAndReabstractedClosure as? MarkDependenceInst {
    convertedAndReabstractedClosures.insert(mdi)
    return 
      markConvertedAndReabstractedClosuresAsUsed(rootClosure: rootClosure, convertedAndReabstractedClosure: mdi.value,
                                                 convertedAndReabstractedClosures: &convertedAndReabstractedClosures)
  }

  fatalError("Unexpected instruction used during closure conversion/reabstraction: \(convertedAndReabstractedClosure)")
}

// ===================== Types ===================== //

/// Represents all the information required to represent a closure in isolation, i.e., outside of a callsite context
/// where the closure may be getting passed as an argument.
///
/// Composed with other information inside a `ClosureArgDescriptor` to represent a closure as an argument at a callsite.
private struct ClosureInfo {
  let closure: SingleValueInstruction
  let lifetimeFrontier: [Instruction]

  init(closure: SingleValueInstruction, lifetimeFrontier: [Instruction]) {
    self.closure = closure
    self.lifetimeFrontier = lifetimeFrontier
  }

}

/// Represents a closure as an argument at a callsite.
private struct ClosureArgDescriptor {
  let closureInfo: ClosureInfo
  /// The index of the closure in the callsite's argument list.
  let closureArgumentIndex: Int
  let parameterInfo: ParameterInfo
  /// This is only needed if we have guaranteed parameters. In most cases
  /// it will have only one element, a return inst.
  var reachableExitBBs: [BasicBlock]
}

/// An intermediate data structure that holds the information about a closure at a callsite that will be specialized on
/// it.
///
/// This type is an implementation detail and exists only to delay the creation of the final `ClosureArgDescriptor`s
/// until the complete lifetime frontier of the corresponding closure has been computed.
private struct IntermediateClosureArgDescriptor {
  let callSite: PartialApplyInst
  let closureArgumentIndex: Int
  let parameterInfo: ParameterInfo
  let reachableExitBBs: [BasicBlock]

  public func intoClosureArgDescriptor(
    closureInfo: ClosureInfo
  ) -> ClosureArgDescriptor {
    return ClosureArgDescriptor(
      closureInfo: closureInfo,
      closureArgumentIndex: closureArgumentIndex,
      parameterInfo: parameterInfo,
      reachableExitBBs: reachableExitBBs
    )
  }
}

/// Represents a callsite containing one or more closure arguments.
private struct CallSite {
  let applySite: ApplySite
  var closureArgDescriptors: [ClosureArgDescriptor] = []
  var silArgIndexToClosureArgDescIndex: [Int: Int] = [:]

  public init(applySite: ApplySite) {
    self.applySite = applySite
  }

  public mutating func appendClosureArgDescriptor(_ descriptor: ClosureArgDescriptor) {
    self.silArgIndexToClosureArgDescIndex[descriptor.closureArgumentIndex] = self.closureArgDescriptors.count
    self.closureArgDescriptors.append(descriptor)
  }

  public mutating func appendClosureArgDescriptors(_ descriptors: [ClosureArgDescriptor]) {
    for descriptor in descriptors {
      self.appendClosureArgDescriptor(descriptor)
    }
  }
}

// ===================== Unit tests ===================== //

let gatherCallSitesTest = FunctionTest("closure_specialize_gather_call_sites") { function, arguments, context in
  print("Specializing closures in function: \(function.name)")
  print("===============================================")
  var callSites = gatherCallSites(in: function, context)

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
