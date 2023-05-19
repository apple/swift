//===--- CalleeAnalysis.swift - the callee analysis -----------------------===//
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

import OptimizerBridging
import SIL

public struct CalleeAnalysis {
  let bridged: BridgedCalleeAnalysis

  static func register() {
    BridgedCalleeAnalysis.registerAnalysis(
      // isDeinitBarrierFn:
      { (inst : BridgedInstruction, bca: BridgedCalleeAnalysis) -> Bool in
        return inst.instruction.isDeinitBarrier(bca.analysis)
      },
      // getMemBehaviorFn
      { (bridgedApply: BridgedInstruction, observeRetains: Bool, bca: BridgedCalleeAnalysis) -> swift.MemoryBehavior in
        let apply = bridgedApply.instruction as! ApplySite
        let e = bca.analysis.getSideEffects(ofApply: apply)
        return e.getMemBehavior(observeRetains: observeRetains)
      }
    )
  }

  public func getCallees(callee: Value) -> FunctionArray? {
    let bridgedFuncs = bridged.getCallees(callee.bridged)
    if bridgedFuncs.isIncomplete() {
      return nil
    }
    return FunctionArray(bridged: bridgedFuncs)
  }

  public func getIncompleteCallees(callee: Value) -> FunctionArray {
    return FunctionArray(bridged: bridged.getCallees(callee.bridged))
  }

  public func getDestructor(ofExactType type: Type) -> Function? {
    let destructors = FunctionArray(bridged: bridged.getDestructors(type.bridged, /*isExactType*/ true))
    if destructors.count == 1 {
      return destructors[0]
    }
    return nil
  }

  public func getDestructors(of type: Type) -> FunctionArray? {
    let bridgedDtors = bridged.getDestructors(type.bridged, /*isExactType*/ false)
    if bridgedDtors.isIncomplete() {
      return nil
    }
    return FunctionArray(bridged: bridgedDtors)
  }

  /// Returns the global (i.e. not argument specific) side effects of an apply.
  public func getSideEffects(ofApply apply: ApplySite) -> SideEffects.GlobalEffects {
    return getSideEffects(ofCallee: apply.callee)
  }

  public func getSideEffects(ofCallee callee: Value) -> SideEffects.GlobalEffects {
    guard let callees = getCallees(callee: callee) else {
      return .worstEffects
    }

    var result = SideEffects.GlobalEffects()
    for callee in callees {
      let calleeEffects = callee.getSideEffects()
      result.merge(with: calleeEffects)
    }
    return result
  }

  /// Returns the argument specific side effects of an apply.
  public func getSideEffects(of apply: ApplySite, forArgument argumentIdx: Int, path: SmallProjectionPath) -> SideEffects.GlobalEffects {
    let calleeArgIdx = apply.calleeArgIndex(callerArgIndex: argumentIdx)
    let convention = apply.getArgumentConvention(calleeArgIndex: calleeArgIdx)
    let argument = apply.arguments[argumentIdx].at(path)

    guard let callees = getCallees(callee: apply.callee) else {
      return .worstEffects.restrictedTo(argument: argument, withConvention: convention)
    }
  
    var result = SideEffects.GlobalEffects()
    for callee in callees {
      let calleeEffects = callee.getSideEffects(forArgument: argument,
                                                atIndex: calleeArgIdx,
                                                withConvention: convention)
      result.merge(with: calleeEffects)
    }
    return result.restrictedTo(argument: argument, withConvention: convention)
  }
}

extension FullApplySite {
  fileprivate func isBarrier(_ analysis: CalleeAnalysis) -> Bool {
    guard let callees = analysis.getCallees(callee: callee) else {
      return true
    }
    return callees.contains { $0.isDeinitBarrier }
  }
}

extension Instruction {
  public final func maySynchronize(_ analysis: CalleeAnalysis) -> Bool {
    if let site = self as? FullApplySite {
      return site.isBarrier(analysis)
    }
    return maySynchronizeNotConsideringSideEffects
  }

  /// Whether lifetime ends of lexical values may safely be hoisted over this
  /// instruction.
  ///
  /// Deinitialization barriers constrain variable lifetimes. Lexical
  /// end_borrow, destroy_value, and destroy_addr cannot be hoisted above them.
  public final func isDeinitBarrier(_ analysis: CalleeAnalysis) -> Bool {
    return mayAccessPointer || mayLoadWeakOrUnowned || maySynchronize(analysis)
  }
}

public struct FunctionArray : RandomAccessCollection, FormattedLikeArray {
  fileprivate let bridged: swift.CalleeList

  public var startIndex: Int { 0 }
  public var endIndex: Int { Int(bridged.getCount()) }

  public subscript(_ index: Int) -> Function {
    return BridgedCalleeAnalysis.getCallee(bridged, index).function
  }
}
// Bridging utilities

extension BridgedCalleeAnalysis {
  public var analysis: CalleeAnalysis { .init(bridged: self) }
}

