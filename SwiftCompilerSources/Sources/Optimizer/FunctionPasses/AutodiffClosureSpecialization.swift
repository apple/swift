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
///     return (%y3, %pb_foo)
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
///     return (%y3, %pb_foo)
/// }
/// ``` 
///
/// Similar complications, as in the existing closure-specialization optimization, exist
/// when populating the specialized Pullback and rewriting the apply instruction in the 
/// VJP. The details of the complications and our work arounds are listed below 
/// (copied over from the existing closure-specialization pass):
///
/// 1. If we support the specialization of closures with multiple user callsites
///    that can be specialized, we need to ensure that any captured values have
///    their reference counts adjusted properly. This implies for every
///    specialized call site, we insert an additional retain for each captured
///    argument with reference semantics. We will pass them in as extra @owned
///    to the specialized function. This @owned will be consumed by the "copy"
///    partial apply that is in the specialized function. Now the partial apply
///    will own those ref counts. This is unapplicable to thin_to_thick_function
///    since they do not have any captured args.
///
/// 2. If the closure was passed in @owned vs if the closure was passed in
///    @guaranteed. If the original closure was passed in @owned, then we know
///    that there is a balancing release for the new "copy" partial apply. But
///    since the original partial apply no longer will have that corresponding
///    -1, we need to insert a release for the old partial apply. We do this
///    right after the old call site where the original partial apply was
///    called. This ensures we do not shrink the lifetime of the old partial
///    apply. In the case where the old partial_apply was passed in at +0, we
///    know that the old partial_apply does not need to have any ref count
///    adjustments. On the other hand, the new "copy" partial apply in the
///    specialized function now needs to be balanced lest we leak. Thus we
///    insert a release right before any exit from the function. This ensures
///    that the release occurs in the epilog after any retains associated with
///    @owned return values.
///
/// 3. In !useLoweredAddresses mode, we do not support specialization of closures
///    with arguments passed using any indirect calling conventions besides
///    @inout and @inout_aliasable.  This is a temporary limitation that goes
///    away with sil-opaque-values.

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

    // If this is not an Autodiff VJP for a differentiable function, exit early.
    if !context.isAutodiffVJP(function: function) {
        return
    }

    log("Gathering callsites for closure specialization in function: \(function.name)")
    log("===============================================")
    let closureSpecializer = AutodiffClosureSpecializer(context)
    var callSites = closureSpecializer.gatherCallSites(function)
    defer {
        callSites.deinitialize()
    }

    callSites.forEach { callSite in
        log("PartialApply call site: \(callSite.applySite)")
        log("Passed in closures: ")
        for index in callSite.closureArgDescriptors.indices {
            var closureArgDescriptor = callSite.closureArgDescriptors[index]
            log("\(index+1). \(closureArgDescriptor.closureInfo.closure)")
        }
    }
    log("\n")
}

// =========== AutodiffClosureSpecializer ========== //

fileprivate extension Dictionary<PartialApplyInst, CallSiteDescriptor> {
    func intoCallSites(_ context: FunctionPassContext) -> Stack<CallSiteDescriptor> {
        var callSites = Stack<CallSiteDescriptor>(context)
        callSites.append(contentsOf: self.values)
        return callSites
    }    
}

struct AutodiffClosureSpecializer: ClosureSpecializer {
    typealias ApplySiteType = ApplySite

    let specializationLevelLimit = 2
    let context: FunctionPassContext

    init(_ context: FunctionPassContext) {
        self.context = context
    }

    /// If the closure obtained from this partial_apply is not the pullback
    /// returned from this autodiff VJP, we have nothing to do.
    private func isPullbackInResultOfAutodiffVJP(pai: PartialApplyInst) -> Bool {
        if let use = pai.result.firstUse {
            if let tupleInst = use.instruction as? TupleInst {
                let maybeReturnedTuple = tupleInst.result
                let vjp = pai.parentFunction
                if let returnInst = vjp.returnInstruction {
                    if maybeReturnedTuple == returnInst.returnedValue {
                        return true
                    }
                }
            }
        }

        return false
    }

    func gatherCallSites(_ caller: Function) -> Stack<CallSiteDescriptor> {
        // We should not look at reabstraction closures twice who we
        // ultimately ended up using as an argument that we specialize on.
        var usedReabstractionClosures = InstructionSet(self.context)
        defer {
            usedReabstractionClosures.deinitialize()
        }

        var paiToCallSiteDesc: [PartialApplyInst: CallSiteDescriptor] = [:]

        for bb in caller.blocks {
            for inst in bb.instructions {
                // If this is not a closure that we support specializing, skip it.
                if !isSupportedClosure(inst) {
                    continue
                }
                let closureInst = inst as! SingleValueInstruction

                // If this is a reabstraction thunk then we have already looked at it, skip it.
                if usedReabstractionClosures.contains(closureInst) {
                    continue
                }
                
                var closureInfo: ClosureInfo? = nil

                // Worklist of operands
                var uses = Array(closureInst.uses)

                // Live range end points.
                var usePoints: [Instruction] = []

                // Set of possible arguments for mark_dependence. The base of a
                // mark_dependence we copy must be available in the specialized function.
                var possibleMarkDependenceBases = ValueSet(self.context)
                defer {
                    possibleMarkDependenceBases.deinitialize()
                }

                if let pai = closureInst as? PartialApplyInst {
                    for arg in pai.arguments {
                        possibleMarkDependenceBases.insert(arg)
                    }
                }

                var haveUsedReabstraction = false

                // Uses may grow in this loop
                for useIndex in 0..<uses.count {
                    let use = uses[useIndex]
                    usePoints.append(use.instruction)

                    // Recurse through conversions
                    if let cfi = use.instruction as? ConvertFunctionInst {
                        uses.append(contentsOf: cfi.uses)
                        possibleMarkDependenceBases.insert(cfi)
                        continue
                    }

                    if let cvt = use.instruction as? ConvertEscapeToNoEscapeInst {
                        uses.append(contentsOf: cvt.uses)
                        possibleMarkDependenceBases.insert(cvt)
                        continue
                    }

                    // Look through reabstraction thunks
                    if let pai = use.instruction as? PartialApplyInst {
                        // Only look at reabstraction thunks if the closure obtained
                        // as a result of this partial_apply is not the pullback returned
                        // from this autodiff VJP.
                        if !isPullbackInResultOfAutodiffVJP(pai: pai) {
                            // Reabstraction can cause series of partial_apply to be emitted. It
                            // is okay to treat these like conversion instructions. Current
                            // restriction: if the partial_apply does not take ownership of its
                            // argument we don't need to analyze which partial_apply to emit
                            // release for (its all of them).
                            let calleeType = pai.callee.type
                            if pai.isPartialApplyOfReabstractionThunk && isSupportedClosure(pai) && 
                                calleeType.isNoEscapeFunction && calleeType.isThickFunction {
                                uses.append(contentsOf: pai.uses)
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
                        if mdi.value == use.value && mdi.value.type.isNoEscapeFunction && mdi.value.type.isThickFunction {
                            uses.append(contentsOf: mdi.uses)
                            continue
                        }
                    }

                    // If the use is not a partial_apply or if it is, but has
                    // substitutions or cannot be optimized, then we can't specialize
                    // it.
                    guard let pai = use.instruction as? PartialApplyInst else {
                        continue
                    }
                    
                    if pai.hasSubstitutions || !pai.canOptimize {
                        continue
                    }

                    if !isPullbackInResultOfAutodiffVJP(pai: pai) {
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
                        return paiToCallSiteDesc.intoCallSites(self.context)
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
                    if !isClosureApplied(in: callee, index: closureArgumentIndex) {
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
                        markReabstractionPartialApplyAsUsed(firstClosure: closureInst, currentClosure: use.value, usedReabstractionClosures: &usedReabstractionClosures)

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
                    guard case Function.NonFailureExitBBsSearchResult.allBlocksUnderstood(let nonFailureExitBBs) = callee.findAllNonFailureExitBBs() else {
                        continue
                    }

                    let onlyHaveThinToThickClosure = closureInst is ThinToThickFunctionInst && !haveUsedReabstraction
                    if (closureParamInfo.convention.isGuaranteed || isClosurePassedTrivially) && !onlyHaveThinToThickClosure {
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
                    let closureCallee = if let paiClosure = closureInst as? PartialApplyInst {
                        paiClosure.referencedFunction!
                    } else {
                        (closureInst as! ThinToThickFunctionInst).referencedFunction!
                    }

                    if self.context.specializationLevel(for: closureCallee) > self.specializationLevelLimit {
                        continue
                    }

                    // Compute the final release points of the closure. We will insert
                    // release of the captured arguments here.
                    if closureInfo == nil {
                        closureInfo = ClosureInfo(closure: closureInst)
                    }

                    // Now we know that it is profitable to specialize this closure's callee
                    // against it. Create a `ClosureArgDescriptor` corresponding to the
                    // closure and add it to the `CallSiteDescriptor` representing its
                    // callee.
                    let closureArgDescriptor = ClosureArgDescriptor(
                        closureInfo: closureInfo!,
                        closureArgumentIndex: closureArgumentIndex,
                        parameterInfo: closureParamInfo,
                        nonFailureExitBBs: nonFailureExitBBs
                    )

                    if var callsiteDesc = paiToCallSiteDesc[pai] {
                        callsiteDesc.addClosureArgDescriptor(closureArgDescriptor)
                        paiToCallSiteDesc[pai] = callsiteDesc
                    } else {
                        paiToCallSiteDesc[pai] = CallSiteDescriptor(
                            applySite: pai,
                            closureArgDescriptors: [closureArgDescriptor],
                            silArgIndexToClosureArgDescIndex: [closureArgumentIndex: 0]
                        )
                    }
                }

                if let closureInfo = closureInfo {
                    var lifetimeFrontier = InstructionRange(begin: closureInst, self.context)
                    defer {
                        lifetimeFrontier.deinitialize()
                    }
                    lifetimeFrontier.insert(contentsOf: usePoints)
                    closureInfo.setLifetimeFrontier(Array(lifetimeFrontier.ends))
                }
            }
        }

        return paiToCallSiteDesc.intoCallSites(self.context)
    }

    func specialize(_ caller: Function) -> Stack<PropagatedClosure> {
        fatalError("Not implemented")
    }
}

// ===================== Utility functions ===================== //

func isSupportedClosure(_ closure: Instruction) -> Bool {
    if !(closure is PartialApplyInst || closure is ThinToThickFunctionInst) {
        return false
    }

    let callee = if closure is PartialApplyInst {
        (closure as! PartialApplyInst).callee
    } else {
        (closure as! ThinToThickFunctionInst).callee
    }

    if callee is FunctionRefInst {
        if let pai = closure as? PartialApplyInst {
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

func isClosureApplied(in callee: Function, index: Int) -> Bool {
    func inner(_ callee: Function, _ index: Int, _ handledFuncs: inout Set<Function>) -> Bool {
        let closureArg = callee.argument(at: index)

        for use in closureArg.uses {
            if let fai = use.instruction as? FullApplySite {
                if fai.callee == closureArg {
                    return true
                }

                if let faiCallee = fai.referencedFunction {
                    if faiCallee.isAvailableExternally && 
                    !handledFuncs.contains(faiCallee) && 
                    handledFuncs.count < recursionBudget {
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

/// Marks any intermediate reabstracted closures corresponding to given closure
/// passed in at a callsite as used. We do not want to look at reabstraction closures 
/// twice as they are covered as part of the final closure that we're passing in at the  
/// callsite.
func markReabstractionPartialApplyAsUsed(firstClosure: Value, currentClosure: Value, usedReabstractionClosures: inout InstructionSet) {
    if currentClosure == firstClosure {
        return
    }

    if let pai = currentClosure as? PartialApplyInst {
        usedReabstractionClosures.insert(pai)
        return markReabstractionPartialApplyAsUsed(firstClosure: firstClosure, currentClosure: pai.callee, usedReabstractionClosures: &usedReabstractionClosures)
    }

    if let cvt = currentClosure as? ConvertFunctionInst {
        usedReabstractionClosures.insert(cvt)
        return markReabstractionPartialApplyAsUsed(firstClosure: firstClosure, currentClosure: cvt.fromFunction, usedReabstractionClosures: &usedReabstractionClosures)
    }

    if let cvt = currentClosure as? ConvertEscapeToNoEscapeInst {
        usedReabstractionClosures.insert(cvt)
        return markReabstractionPartialApplyAsUsed(firstClosure: firstClosure, currentClosure: cvt.fromFunction, usedReabstractionClosures: &usedReabstractionClosures)
    }

    if let mdi = currentClosure as? MarkDependenceInst {
        usedReabstractionClosures.insert(mdi)
        return markReabstractionPartialApplyAsUsed(firstClosure: firstClosure, currentClosure: mdi.value, usedReabstractionClosures: &usedReabstractionClosures)
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
class ClosureInfo {
    let closure: SingleValueInstruction
    var lifetimeFrontier: [Instruction]?

    init(closure: SingleValueInstruction) {
        self.closure = closure
        self.lifetimeFrontier = nil
    }

    public func setLifetimeFrontier(_ frontier: [Instruction]) {
        if self.lifetimeFrontier == nil {
            self.lifetimeFrontier = frontier
        } 
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

/// Represents a callsite containing one or more closure arguments.
struct CallSiteDescriptor {
    let applySite: ApplySite
    var closureArgDescriptors: [ClosureArgDescriptor]
    var silArgIndexToClosureArgDescIndex: [Int: Int]

    public mutating func addClosureArgDescriptor(_ descriptor: ClosureArgDescriptor) {
        self.silArgIndexToClosureArgDescIndex[descriptor.closureArgumentIndex] = self.closureArgDescriptors.count
        self.closureArgDescriptors.append(descriptor)
    }
}

/// A type capable of performing closure specialization on SIL functions
protocol ClosureSpecializer {
    typealias PropagatedClosure = SingleValueInstruction
    associatedtype ApplySiteType

    func gatherCallSites(_ caller: Function) -> Stack<CallSiteDescriptor>
    func specialize(_ caller: Function) -> Stack<PropagatedClosure>
}


// ===================== Unit tests ===================== //
let gatherCallSitesTest = FunctionTest("closure_specialize_gather_call_sites") {
    function, arguments, context in

    print("Specializing closures in function: \(function.name)")
    print("===============================================")
    let closureSpecializer = AutodiffClosureSpecializer(context)
    var callSites = closureSpecializer.gatherCallSites(function)
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