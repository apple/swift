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

public struct ApplyOperands {
  public static let calleeOperandIndex: Int = 0
  public static let firstArgumentIndex = 1
}

public protocol ApplySite : Instruction {
  var operands: OperandArray { get }
  var numArguments: Int { get }
  var substitutionMap: SubstitutionMap { get }
  
  /// Converts an argument index of the apply to the corresponding argument index of the callee.
  ///
  /// For a FullApplySite this is always a 1-to-1 mapping.
  /// For a `partial_apply` the callee index can be higher than the caller's argument index
  /// because the arguments to `partial_apply` are a suffix of the callee.
  ///
  /// Example:
  /// ```
  /// func callee(a, b, c, d, e) { }
  ///
  /// %pa = partial_apply @callee(c, d, e)
  /// // caller indices:          0, 1, 2
  /// // callee indices:          2, 3, 4
  ///
  ///  %a = apply         %pa    (a, b)
  /// // caller indices:          0, 1
  /// // callee indices:          0, 1
  /// ```
  func calleeArgIndex(callerArgIndex: Int) -> Int

  /// Converts an argument index of a callee to the corresponding argument index of the apply.
  ///
  /// If the apply does not actually apply that argument, it returns nil.
  /// Otherwise, for a FullApplySite this is always a 1-to-1 mapping.
  /// For a `partial_apply` the caller index can be lower than the callee's argument index
  /// because the arguments to `partial_apply` are a suffix of the callee.
  ///
  /// Example:
  /// ```
  ///                func callee(a, b, c, d, e) { }
  /// // callee indices:         0, 1, 2, 3, 4
  /// // caller indices in %pa:  -, -, 0, 1, 2     ("-" == nil)
  /// // caller indices in %a:   0, 1, -, -, -
  ///
  /// %pa = partial_apply @callee(c, d, e)
  ///  %a = apply         %pa    (a, b)
  /// ```
  func callerArgIndex(calleeArgIndex: Int) -> Int?

  func getArgumentConvention(calleeArgIndex: Int) -> ArgumentConvention
}

extension ApplySite {
  public var callee: Value { operands[ApplyOperands.calleeOperandIndex].value }

  /// Returns the subset of operands which are argument operands.
  ///
  /// This does not include the callee function operand.
  public var argumentOperands: OperandArray {
    let numArgs = bridged.ApplySite_getNumArguments()
    let offset = ApplyOperands.firstArgumentIndex
    return operands[offset..<(numArgs + offset)]
  }

  /// Returns the subset of operand values which are arguments.
  ///
  /// This does not include the callee function operand.
  public var arguments: LazyMapSequence<OperandArray, Value> {
    argumentOperands.lazy.map { $0.value }
  }

  public var substitutionMap: SubstitutionMap {
    SubstitutionMap(bridged.ApplySite_getSubstitutionMap())
  }

  /// Returns the argument index of an operand.
  ///
  /// Returns nil if 'operand' is not an argument operand. This is the case if
  /// it's the callee function operand.
  public func argumentIndex(of operand: Operand) -> Int? {
    let opIdx = operand.index
    if opIdx >= ApplyOperands.firstArgumentIndex &&
       opIdx <= ApplyOperands.firstArgumentIndex + numArguments {
      return opIdx - ApplyOperands.firstArgumentIndex
    }
    return nil
  }

  /// Returns true if `operand` is the callee function operand and not am argument operand.
  public func isCalleeOperand(_ operand: Operand) -> Bool {
    return operand.index < ApplyOperands.firstArgumentIndex
  }
  
  public func getArgumentConvention(calleeArgIndex: Int) -> ArgumentConvention {
    return bridged.ApplySite_getArgumentConvention(calleeArgIndex).convention
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
}

public protocol FullApplySite : ApplySite {
  var singleDirectResult: Value? { get }
}

extension FullApplySite {
  public func calleeArgIndex(callerArgIndex: Int) -> Int {
    assert(callerArgIndex >= 0 && callerArgIndex < numArguments)
    return callerArgIndex
  }

  public func callerArgIndex(calleeArgIndex: Int) -> Int? {
    if calleeArgIndex < numArguments {
      return calleeArgIndex
    }
    return nil
  }

  /// The number of indirect out arguments.
  ///
  /// 0 if the callee has a direct or no return value and 1, if it has an indirect return value.
  public var numIndirectResultArguments: Int {
    return bridged.FullApplySite_numIndirectResultArguments()
  }
}
