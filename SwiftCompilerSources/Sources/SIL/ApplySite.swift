//===--- ApplySite.swift - Defines the ApplySite protocols ----------------===//
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

import SILBridging

/// Argument conventions indexed on an apply's operand.
///
/// `partial_apply` operands correspond to a suffix of the callee
/// arguments.
///
/// Example:
/// ```
/// func callee(a, b, c, d, e) { }
///
/// %pa = partial_apply @callee(c, d, e)
/// // operand indices:         1, 2, 3
/// // callee indices:          2, 3, 4
///
///  %a = apply         %pa    (a, b)
/// // operand indices:         1, 2
/// // callee indices:          0, 1
/// ```
public struct ApplyOperandConventions : Collection {
  public static let calleeIndex: Int = 0
  public static let firstArgumentIndex = 1

  /// Callee's argument conventions indexed on the function's arguments.
  public let calleeArgumentConventions: ArgumentConventions

  public let unappliedArgumentCount: Int

  public var appliedArgumentCount: Int {
    calleeArgumentConventions.count - unappliedArgumentCount
  }

  public var startIndex: Int { ApplyOperandConventions.firstArgumentIndex }

  public var endIndex: Int { ApplyOperandConventions.firstArgumentIndex + appliedArgumentCount }

  public func index(after index: Int) -> Int {
    return index + 1
  }

  public subscript(_ operandIndex: Int) -> ArgumentConvention {
    return calleeArgumentConventions[
      calleeArgumentIndex(ofOperandIndex: operandIndex)!]
  }

  public subscript(result operandIndex: Int) -> ResultInfo? {
    return calleeArgumentConventions[result:
      calleeArgumentIndex(ofOperandIndex: operandIndex)!]
  }

  public subscript(parameter operandIndex: Int) -> ParameterInfo? {
    return calleeArgumentConventions[parameter:
      calleeArgumentIndex(ofOperandIndex: operandIndex)!]
  }

  public subscript(resultDependsOn operandIndex: Int)
    -> LifetimeDependenceConvention? {
    return calleeArgumentConventions[resultDependsOn:
      calleeArgumentIndex(ofOperandIndex: operandIndex)!]
  }

  public var firstParameterOperandIndex: Int {
    return ApplyOperandConventions.firstArgumentIndex +
      calleeArgumentConventions.firstParameterIndex
  }

  // TODO: rewrite uses of this API to avoid manipulating integer
  // indices, and make this private. No client should have multiple
  // integer indices, some of which are caller indices, and some of
  // which are callee indices.
  public func calleeArgumentIndex(ofOperandIndex index: Int) -> Int? {
    let callerArgIdx = index - startIndex
    if callerArgIdx < 0 {
      return nil
    }
    let calleeArgIdx = callerArgIdx + unappliedArgumentCount
    assert(calleeArgIdx < calleeArgumentConventions.count,
           "invalid operand index")
    return calleeArgIdx
  }

  // TODO: this should be private.
  public func calleeArgumentIndex(of operand: Operand) -> Int? {
    calleeArgumentIndex(ofOperandIndex: operand.index)
  }
}

public protocol ApplySite : Instruction {
  var operands: OperandArray { get }
  var numArguments: Int { get }
  var substitutionMap: SubstitutionMap { get }

  var unappliedArgumentCount: Int { get }
}

extension ApplySite {
  public var callee: Value { operands[ApplyOperandConventions.calleeIndex].value }

  public var hasSubstitutions: Bool {
    return substitutionMap.hasAnySubstitutableParams
  }

  public var isAsync: Bool {
    return callee.type.isAsyncFunction
  }

  public var referencedFunction: Function? {
    if let fri = callee as? FunctionRefInst {
      return fri.referencedFunction
    }
    return nil
  }

  public func hasSemanticsAttribute(_ attr: StaticString) -> Bool {
    if let callee = referencedFunction {
      return callee.hasSemanticsAttribute(attr)
    }
    return false
  }

  public var isCalleeNoReturn: Bool {
    bridged.ApplySite_isCalleeNoReturn()  
  }

  public var isCalleeTrapNoReturn: Bool {
    referencedFunction?.isTrapNoReturn ?? false
  }

  /// Returns the subset of operands which are argument operands.
  ///
  /// This does not include the callee function operand.
  public var argumentOperands: OperandArray {
    let numArgs = bridged.ApplySite_getNumArguments()
    let offset = ApplyOperandConventions.firstArgumentIndex
    return operands[offset..<(numArgs + offset)]
  }

  /// Returns the subset of operands that are parameters. This does
  /// not include indirect results. This does include 'self'.
  public var parameterOperands: OperandArray {
    let firstParamIdx =
      operandConventions.calleeArgumentConventions.firstParameterIndex
    let argOpers = argumentOperands // bridged call
    return argOpers[firstParamIdx..<argOpers.count]
  }

  /// Returns the subset of operand values which are arguments.
  ///
  /// This does not include the callee function operand.
  public var arguments: LazyMapSequence<OperandArray, Value> {
    argumentOperands.values
  }

  /// Indirect results including the error result.
  public var indirectResultOperands: OperandArray {
    let ops = operandConventions
    return operands[ops.startIndex..<ops.firstParameterOperandIndex]
  }

  public func isIndirectResult(operand: Operand) -> Bool {
    let idx = operand.index
    let ops = operandConventions
    return idx >= ops.startIndex && idx < ops.firstParameterOperandIndex
  }

  public var substitutionMap: SubstitutionMap {
    SubstitutionMap(bridged.ApplySite_getSubstitutionMap())
  }

  public var calleeArgumentConventions: ArgumentConventions {
    ArgumentConventions(convention: functionConvention)
  }

  public var operandConventions: ApplyOperandConventions {
    ApplyOperandConventions(
      calleeArgumentConventions: calleeArgumentConventions,
      unappliedArgumentCount: bridged.PartialApply_getCalleeArgIndexOfFirstAppliedArg())
  }

  /// Returns true if `operand` is the callee function operand and not am argument operand.
  public func isCallee(operand: Operand) -> Bool {
    operand.index == ApplyOperandConventions.calleeIndex
  }

  public func convention(of operand: Operand) -> ArgumentConvention? {
    let idx = operand.index
    return idx < operandConventions.startIndex ? nil : operandConventions[idx]
  }

  public func result(for operand: Operand) -> ResultInfo? {
    let idx = operand.index
    return idx < operandConventions.startIndex ? nil
      : operandConventions[result: idx]
  }

  public func parameter(for operand: Operand) -> ParameterInfo? {
    let idx = operand.index
    return idx < operandConventions.startIndex ? nil
      : operandConventions[parameter: idx]
  }

  public func resultDependence(on operand: Operand)
    -> LifetimeDependenceConvention? {
    let idx = operand.index
    return idx < operandConventions.startIndex ? nil
      : operandConventions[resultDependsOn: idx]
  }

  public var hasResultDependence: Bool {
    functionConvention.resultDependencies != nil
  }

  public var yieldConventions: YieldConventions {
    YieldConventions(convention: functionConvention)
  }

  public func convention(of yield: MultipleValueInstructionResult)
    -> ArgumentConvention {
    assert(yield.definingInstruction == self)
    return yieldConventions[yield.index]
  }

  /// Converts an argument index of a callee to the corresponding apply operand.
  ///
  /// If the apply does not actually apply that argument, it returns nil.
  ///
  /// Example:
  /// ```
  ///                 func callee(v, w, x, y, z) { }
  /// // caller operands in %pa:  -, -, c, d, e     ("-" == nil)
  /// // caller operands in %a:   a, b, -, -, -
  ///
  /// %pa = partial_apply @callee(c, d, e)
  ///  %a = apply         %pa    (a, b)
  /// ```
  ///
  /// TODO: delete this API and rewrite the users. 
  public func operand(forCalleeArgumentIndex calleeArgIdx: Int) -> Operand? {
    let callerArgIdx = calleeArgIdx - operandConventions.unappliedArgumentCount
    guard callerArgIdx >= 0 && callerArgIdx < numArguments else { return nil }
    return argumentOperands[callerArgIdx]
  }

  /// Returns the argument index of an operand.
  ///
  /// Returns nil if 'operand' is not an argument operand. This is the case if
  /// it's the callee function operand.
  ///
  /// Warning: the returned integer can be misused as an index into
  /// the wrong collection. Replace uses of this API with safer APIs.
  ///
  /// TODO: delete this API and rewrite the users. 
  public func calleeArgumentIndex(of operand: Operand) -> Int? {
    operandConventions.calleeArgumentIndex(of: operand)
  }
}

extension ApplySite {
  private var functionConvention: FunctionConvention {
    FunctionConvention(for: bridged.ApplySite_getSubstitutedCalleeType(),
                       in: parentFunction)
  }
}

public protocol FullApplySite : ApplySite {
  var singleDirectResult: Value? { get }
}

extension FullApplySite {
  public var unappliedArgumentCount: Int { 0 }

  /// The number of indirect out arguments.
  ///
  /// 0 if the callee has a direct or no return value and 1, if it has an indirect return value.
  ///
  /// FIXME: This is incorrect in two cases: it does not include the
  /// indirect error result, and, prior to address lowering, does not
  /// include pack results.
  public var numIndirectResultArguments: Int {
    return bridged.FullApplySite_numIndirectResultArguments()
  }

  /// The direct result or yields produced by this apply. This does
  /// not include any potential results returned by a coroutine
  /// (end_apply results).
  public var resultOrYields: SingleInlineArray<Value> {
    var values = SingleInlineArray<Value>()
    if let beginApply = self as? BeginApplyInst {
      beginApply.yieldedValues.forEach { values.push($0) }
    } else {
      let result = singleDirectResult!
      if !result.type.isEmpty(in: parentFunction) {
        values.push(result)
      }
    }
    return values
  }
}
