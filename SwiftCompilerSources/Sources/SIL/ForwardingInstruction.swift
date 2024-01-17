//===--- ForwardingInstruction.swift - forwarding instruction protocols ---===//
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

import SILBridging

/// An instruction that forwards ownership from operands to results.
///
/// Forwarding instructions are not allowed to store the value or
/// propagate its bits in any way that isn't tracked by its
/// results. Passes assume that a forwarding value is nonescaping as
/// long as its results are nonescaping.
public protocol ForwardingInstruction : Instruction {
  var singleForwardedOperand: Operand? { get }

  /// Return true if the result has the same object identify, and therefore the same reference counting semantics as the
  /// source. This is true for most forwarding operations unless they extract or aggregate components.
  var preservesIdentity: Bool { get }

  /// Return true if the forwarded value has the same representation. If true, then the result can be mapped to the same storage without a move or copy.
  var preservesRepresentation: Bool { get }

  var canForwardGuaranteedValues: Bool { get }

  var canForwardOwnedValues: Bool { get }
}

extension ForwardingInstruction {
  public var forwardedOperands: OperandArray {
    // Some instructions have multiple real operands but only forward one.
    if let singleForwardingOp = singleForwardedOperand {
      return OperandArray(base: singleForwardingOp, count: 1)
    }
    // All others forward all operands (for enum, this may be zero operands).
    return operands
  }

  public var forwardedResults: ForwardedResults {
    ForwardedResults(inst: self)
  }

  /// If forwarding ownership is owned, then the instruction moves an owned operand to its result, ending its lifetime. If forwarding ownership is guaranteed, then the instruction propagates the lifetime of its borrows operand through its result.
  ///
  /// The resulting forwarded value's ownership (Value.ownership) is not identical to the instruction's forwarding ownership property. It differs when the result is trivial type. e.g. an owned or guaranteed value can be cast to a trivial type using owned or guaranteed forwarding.
  public var forwardingOwnership: Ownership {
    Ownership(bridged: bridged.ForwardingInst_forwardingOwnership())
  }
  
  /// A forwarding instruction preserves reference counts if it has a dynamically non-trivial result in which all references are forwarded from the operand.
  ///
  /// A cast can only forward guaranteed values if it preserves reference counts. Such casts cannot release any references within their operand's value and cannot retain any references owned by their result.
  public var preservesReferenceCounts: Bool {
    bridged.ForwardingInst_preservesOwnership()
  }
}

extension Value {
  // If this value is produced by a ForwardingInstruction, return that instruction. This is convenient for following the forwarded value chain.
  // Unlike definingInstruction, a value's forwardingInstruction is not necessarily a valid insertion point. 
  public var forwardingInstruction: ForwardingInstruction? {
    if let inst = definingInstruction {
      return inst as? ForwardingInstruction
    }
    if let termResult = TerminatorResult(self) {
      return termResult.terminator as? ForwardingInstruction
    }
    return nil
  }
}

//===----------------------------------------------------------------------===//
//                      singleForwardedOperand
//===----------------------------------------------------------------------===//

extension ForwardingInstruction {
  // See ForwardingOperation::getSingleForwardingOperand().
  public var singleForwardedOperand: Operand? {
    let definedOps = self.definedOperands
    assert(definedOps.count == 1, "expected single operand for forwarding")
    return definedOps[0]
  }
}

extension ConversionInstruction {
  public var singleForwardedOperand: Operand? { operand }
}

//===----------------------------------------------------------------------===//
//                         forwardedResults
//===----------------------------------------------------------------------===//

public struct ForwardedResults : Collection {
  private let inst: Instruction
  private let maxResults: Int

  fileprivate init(inst: ForwardingInstruction) {
    self.inst = inst
    if let ti = inst as? TermInst {
      self.maxResults = ti.successors.count
    } else {
      self.maxResults = inst.results.count
    }
  }

  public var startIndex: Int { skipEmptyResults(at: 0) }

  public var endIndex: Int { maxResults }

  public func index(after index: Int) -> Int {
    return skipEmptyResults(at: index + 1)
  }

  public subscript(_ index: Int) -> Value {
    if let ti = inst as? TermInst {
      return getTerminatorResult(termInst: ti, index: index)!
    }
    return inst.results[index]
  }

  private func skipEmptyResults(at index: Int) -> Int {
    guard let ti = inst as? TermInst else { return index }
    var next = index
    while next != endIndex {
      if getTerminatorResult(termInst: ti, index: next) != nil { break }
      next += 1
    }
    return next
  }

  // Forwarding terminators have a zero or one result per successor.
  private func getTerminatorResult(termInst: TermInst,
    index: Int) -> Argument? {
    let succ = termInst.successors[index]
    guard succ.arguments.count == 1 else {
      // The default enum payload may be empty.
      assert(succ.arguments.count == 0, "terminator must forward a single value")
      return nil
    }
    return succ.arguments[0]
  }
}

//===----------------------------------------------------------------------===//
//                         conforming instructions
//===----------------------------------------------------------------------===//

// -----------------------------------------------------------------------------
// Instructions with multiple forwarded operands have nil singleForwardedOperand.

extension StructInst : ForwardingInstruction {
  public var singleForwardedOperand: Operand? { nil }

  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension TupleInst : ForwardingInstruction {
  public var singleForwardedOperand: Operand? { nil }

  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension LinearFunctionInst : ForwardingInstruction {
  public var singleForwardedOperand: Operand? { nil }

  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension DifferentiableFunctionInst : ForwardingInstruction {
  public var singleForwardedOperand: Operand? { nil }

  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

// -----------------------------------------------------------------------------
// Instructions with a singleForwardedOperand and additional operands.

extension MarkDependenceInst : ForwardingInstruction {
  public var singleForwardedOperand: Operand? {
    return valueOperand
  }

  public var preservesIdentity: Bool { true }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension RefToBridgeObjectInst : ForwardingInstruction {
  public var singleForwardedOperand: Operand? {
    return convertedOperand
  }

  public var preservesIdentity: Bool { true }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension TuplePackExtractInst : ForwardingInstruction {
  public var singleForwardedOperand: Operand? { return tupleOperand }

  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { false }
}

// -----------------------------------------------------------------------------
// conversion instructions

/// An instruction that forwards a single value to a single result.
///
/// For legacy reasons, some ForwardingInstructions that fit the
/// SingleValueInstruction and UnaryInstruction requirements are not
/// considered ConversionInstructions because certain routines do not
/// want to see through them (InitExistentialValueInst,
/// DeinitExistentialValueInst, OpenExistentialValueInst,
/// OpenExistentialValueInst). This most likely has to do with
/// type-dependent operands, although any ConversionInstruction should
/// support type-dependent operands.
public protocol ConversionInstruction : SingleValueInstruction, UnaryInstruction, ForwardingInstruction {}

extension ConversionInstruction {
  /// Conversion instructions naturally preserve identity as long as they preserve reference counts because they do not
  /// aggregate or disaggregate values. They can only lose identity by destroying the source object and instantiating a
  /// new object, like certain bridging casts do.
  public var preservesIdentity: Bool { preservesReferenceCounts }
}

extension MarkUnresolvedNonCopyableValueInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension MarkUninitializedInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { false }
  public var canForwardOwnedValues: Bool { true }
}

extension ConvertFunctionInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension ThinToThickFunctionInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension CopyableToMoveOnlyWrapperValueInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension MoveOnlyWrapperToCopyableValueInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension MoveOnlyWrapperToCopyableBoxInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension UpcastInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension UncheckedRefCastInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension UnconditionalCheckedCastInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension BridgeObjectToRefInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension UncheckedValueCastInst : ConversionInstruction {
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

// -----------------------------------------------------------------------------
// other forwarding instructions

extension EnumInst : ForwardingInstruction {
  public var singleForwardedOperand: Operand? {
    return operand // nil for an enum with no payload
  }

  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension DestructureTupleInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension DestructureStructInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension FunctionExtractIsolationInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { false }
}

extension InitExistentialRefInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension OpenExistentialRefInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension OpenExistentialValueInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension OpenExistentialBoxValueInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension StructExtractInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { false }
}

extension TupleExtractInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { false }
}

extension DifferentiableFunctionExtractInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { false }
}

extension CheckedCastBranchInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension UncheckedEnumDataInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension SwitchEnumInst : ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension MarkUnresolvedReferenceBindingInst : ForwardingInstruction {
  public var preservesIdentity: Bool { true }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension LinearFunctionExtractInst: ForwardingInstruction {
  public var preservesIdentity: Bool { false }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { true }
}

extension BorrowedFromInst: ForwardingInstruction {
  public var singleForwardedOperand: Operand? { operands[0] }
  public var preservesIdentity: Bool { true }
  public var preservesRepresentation: Bool { true }
  public var canForwardGuaranteedValues: Bool { true }
  public var canForwardOwnedValues: Bool { false }
}

// -----------------------------------------------------------------------------
// ownership transition instructions

/// An instruction that transfers lifetime dependence from a single operand to a single result. The operand and result
/// have the same identity, but they are not part of the same forwarded lifetime:
/// copy_value, move_value, begin_borrow.
///
/// OwnershipTransitionInstructions always preserve the identity of the source. See swift::isIdentityPreservingRefCast.
public protocol OwnershipTransitionInstruction: UnaryInstruction {
  var ownershipResult: Value { get }
}

extension OwnershipTransitionInstruction where Self: SingleValueInstruction {
  public var ownershipResult: Value { self }
}

// CopyingInstruction implies OwnershipTransitionInstruction

extension MoveValueInst: OwnershipTransitionInstruction {}

extension BeginBorrowInst: OwnershipTransitionInstruction {}

extension BeginCOWMutationInst: OwnershipTransitionInstruction {
  public var ownershipResult: Value { instanceResult }
}

extension EndCOWMutationInst: OwnershipTransitionInstruction {}

extension EndInitLetRefInst: OwnershipTransitionInstruction {}

extension BeginDeallocRefInst: OwnershipTransitionInstruction {}
