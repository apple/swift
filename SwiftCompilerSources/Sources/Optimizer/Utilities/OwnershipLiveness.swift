//===--- OwnershipLiveness.swift - Utilities for ownership liveness -------===//
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
//
// Utilities that specify ownership SSA (OSSA) lifetimes.
//
// TODO: Implement ExtendedLinearLiveness. This requires
// MultiDefPrunedLiveness, which is not supported by InstructionRange.
//
// TODO: Move this all into SIL, along with DominatorTree. OSSA
// lifetimes and dominance are part of SIL semantics, and need to be
// verified. Remove uses of FunctionPassContext.
//
//===----------------------------------------------------------------------===//

import SIL

private let verbose = false

private func log(_ message: @autoclosure () -> String) {
  if verbose {
    print("### \(message())")
  }
}

/// Compute liveness and return a range, which the caller must deinitialize.
///
/// `definingValue` must introduce an OSSA lifetime. It may be either
/// an owned value or introduce a borrowed value (BeginBorrowValue),
/// including:
///
/// 1. Owned non-phi values
/// 2. Owned phi values
/// 3. Borrow scope introducers: begin_borrow/load_borrow
/// 4. Reborrows: guaranteed phi that ends its operand's borrow scope and
///    requires post-dominating scope-ending uses (end_borrow or reborrow)
///
/// `definingValue`'s lifetime must already complete on all paths
/// (a.k.a linear). Only lifetime-ending operations generate liveness.
///
/// `definingValue` dominates the range. Forwarding and phi uses do
/// not extend the lifetime.
///
/// This is the simplest OSSA liveness analysis. It assumes that OSSA
/// lifetime completion has already run on `definingValue`, and it
/// cannot fix OSSA lifetimes after a transformation.
func computeLinearLiveness(for definingValue: Value, _ context: Context)
  -> InstructionRange {

  assert(definingValue.ownership == .owned
    || BeginBorrowValue(definingValue) != nil,
    "value must define an OSSA lifetime")

  // InstructionRange cannot directly represent the beginning of the block
  // so we fake it with getRepresentativeInstruction().
  var range = InstructionRange(for: definingValue, context)

  // Compute liveness.
  definingValue.lookThroughBorrowedFromUser.uses.endingLifetime.forEach {
    range.insert($0.instruction)
  }
  return range
}

typealias InnerScopeHandler = (Value) -> WalkResult

/// Compute liveness and return a range, which the caller must deinitialize.
///
/// An OSSA lifetime begins with a single "defining" value, which must be owned, or must begin a borrow scope. A
/// complete OSSA lifetime has a linear lifetime, meaning that it has a lifetime-ending use on all paths. Interior
/// liveness computes liveness without assuming the lifetime is complete. To do this, it must find all "use points" and
/// prove that the defining value is never propagated beyond those points. This is used to initially complete OSSA
/// lifetimes and fix them after transformations that's don't preserve OSSA.
///
/// The caller must check that `definingValue` has no pointer escape before calling this.
///
/// Invariants:
///
/// - The definition dominates all use points.
///
/// - Liveness does not extend beyond lifetime-ending operations
/// (a.k.a. affine lifetimes).
///
/// - All inner scopes are complete. (Use `innerScopeHandler` to complete them or bail-out).
func computeInteriorLiveness(for definingValue: Value, _ context: FunctionPassContext,
                             innerScopeHandler: InnerScopeHandler? = nil) -> InstructionRange {
  let result = InteriorLivenessResult.compute(for: definingValue, ignoreEscape: false, context)
  switch result.pointerStatus {
  case .nonescaping:
    break
  case let .escaping(operands):
    fatalError("""
                 check findPointerEscape() before computing interior liveness.
                 Pointer escape: \(operands[0].instruction)
                 """)
  case let .unknown(operand):
    fatalError("Unrecognized SIL address user \(operand.instruction)")
  }
  return result.range
}

/// Compute known liveness and return a range, which the caller must deinitialize.
///
/// This computes a minimal liveness, ignoring pointer escaping uses.
func computeKnownLiveness(for definingValue: Value, _ context: FunctionPassContext) -> InstructionRange {
  return InteriorLivenessResult.compute(for: definingValue, ignoreEscape: true, context).range
}

/// If any interior pointer may escape, then record the first instance here. If 'ignoseEscape' is true, this
/// immediately aborts the walk, so further instances are unavailable.
///
/// .escaping may either be a non-address operand with
/// .pointerEscape ownership, or and address operand that escapes
/// the address (address_to_pointer).
///
/// .unknown is an address operand whose user is unrecognized.
enum InteriorPointerStatus: CustomDebugStringConvertible {
  case nonescaping
  case escaping(SingleInlineArray<Operand>)
  case unknown(Operand)

  mutating func setEscaping(operand: Operand) {
    switch self {
    case .nonescaping:
      self = .escaping(SingleInlineArray(element: operand))
    case let .escaping(oldOperands):
      var newOperands = SingleInlineArray<Operand>()
      newOperands.append(contentsOf: oldOperands)
      newOperands.append(operand)
      self = .escaping(newOperands)
    case .unknown:
      break
    }
  }

  var debugDescription: String {
    switch self {
    case .nonescaping:
      return "No pointer escape"
    case let .escaping(operands):
      return "Pointer escapes: " + operands.map({ "\($0)" }).joined(separator: "\n                 ")
    case let .unknown(operand):
      return "Unknown use: \(operand)"
    }
  }
}

struct InteriorLivenessResult: CustomDebugStringConvertible {
  let success: WalkResult
  let range: InstructionRange
  let pointerStatus: InteriorPointerStatus

  static func compute(for definingValue: Value, ignoreEscape: Bool = false,
                      _ context: FunctionPassContext,
                      innerScopeHandler: InnerScopeHandler? = nil) -> InteriorLivenessResult {

    assert(definingValue.ownership == .owned || BeginBorrowValue(definingValue) != nil,
           "value must define an OSSA lifetime")

    var range = InstructionRange(for: definingValue, context)

    var visitor = InteriorUseWalker(definingValue: definingValue, ignoreEscape: ignoreEscape, context) {
      range.insert($0.instruction)
      return .continueWalk
    }
    defer { visitor.deinitialize() }
    visitor.innerScopeHandler = innerScopeHandler
    let success = visitor.visitUses()
    assert(visitor.unenclosedPhis.isEmpty, "missing adjacent phis")
    let result = InteriorLivenessResult(success: success, range: range, pointerStatus: visitor.pointerStatus)
    log("Interior liveness for: \(definingValue)\n\(result)")
    return result
  }

  var debugDescription: String {
    "\(success)\n\(range)\n\(pointerStatus)"
  }
}

/// Classify ownership uses. This reduces operand ownership to a
/// visitor API that can be used by def-use walkers to ensure complete
/// handling of all legal SIL patterns.
///
/// Code that relies on the ownership effect of a use should conform
/// to this visitor. This facilitates correct handling of special cases
/// involving borrow scopes and interior pointers.
///
/// The top-level entry points are:
/// - `classify(operand:)`
/// - `visitUsesOfOuter(value:)`
///
/// The implementation may recursively call back to the top-level
/// entry points. Additionally, the implementation may recurse into inner
/// borrow scopes, skipping over the uses within inner scopes using:
/// - `visitInnerBorrowUses(of:)`
/// - `visitUsesOfInner(value:)`
///
/// Visitors implement:
///
/// - ownershipLeafUse(of:isInnerlifetime:)
/// - forwardingUse(of:isInnerlifetime:)
/// - interiorPointerUse(of:into:)
/// - pointerEscapingUse(of:)
/// - dependentUse(of:into:)
/// - borrowingUse(of:by:)
/// - reborrowingUse(of:isInnerlifetime:)
///
/// This only visits the first level of uses. The implementation may
/// transitively visit forwarding operations in its implementation of
/// `forwardingUse(of:isInnerlifetime:)` and
/// `reborrowingUse(of:isInnerlifetime:)`.
///
/// `isInnerlifetime` indicates whether the value being used is
/// defined by the "outer" OSSA lifetime or an inner borrow scope.
/// When the OwnershipUseVisitor is invoked on an outer value
/// (visitUsesOfOuter(value:)), it visits all the uses of that value
/// and also visits the lifetime-ending uses of any inner borrow
/// scopes. This provides a complete set of liveness "use points":
///
///   %0 = begin_borrow %outerValue
///   %1 = begin_borrow %0
///   end_borrow %1        // inner "use point" of %0
///   end_borrow %0        // outer use of %0
///
/// This becomes more complicated with reborrows and closures. The
/// implementation can simply rely on isInnerLifetime to know whether
/// the value being used is part of the outer lifetimes vs. its inner
/// lifetimes. This is important, for example, if the implementation
/// wants to know if the use ends the lifetime of the outer value.
///
/// Visitor implementations treat inner and outer uses differently. It
/// may, for example, assume that inner lifetimes are complete
/// and therefore only care about the lifetime-ending uses.
protocol OwnershipUseVisitor {
  var context: Context { get }

  /// A non-forwarding use.
  ///
  /// `isInnerLifetime` indicates whether `operand` uses the original
  /// OSSA lifetime. This use ends the original lifetime if
  /// (!isInnerLifetime && use.endsLifetime).
  mutating func ownershipLeafUse(of operand: Operand, isInnerLifetime: Bool)
    -> WalkResult

  /// A forwarding operand.
  ///
  /// Use ForwardingInstruction or ForwardingDefUseWalker to handle
  /// downstream uses.
  ///
  /// If `isInnerLifetime` is true, then the value depends on an inner borrow.
  mutating func forwardingUse(of operand: Operand, isInnerLifetime: Bool)
    -> WalkResult

  /// A use that projects an address.
  mutating func interiorPointerUse(of: Operand, into address: Value)
    -> WalkResult

  /// A use that escapes information from its operand's value.
  ///
  /// Note: this may not find all relevant pointer escapes, such as
  /// from owned forwarded values. Clients should generally check
  /// findPointerEscape() before relying on a liveness result and
  /// implement this as a fatalError.
  mutating func pointerEscapingUse(of operand: Operand) -> WalkResult

  /// A use that creates an implicit borrow scope over the lifetime of
  /// an owned dependent value. The operand owership is .borrow, but
  /// there are no explicit scope-ending operations. Instead
  /// BorrowingInstruction.scopeEndingOperands will return the final
  /// consumes in the dependent value's forwaring chain.
  mutating func dependentUse(of operand: Operand, into value: Value)
    -> WalkResult

  /// A use that is scoped to an inner borrow scope.
  ///
  /// Call `visitInnerBorrowUses(of:)` to recursively classify any
  /// scope-ending uses and forwarded dependent values.
  mutating func borrowingUse(of operand: Operand,
                             by borrowInst: BorrowingInstruction) -> WalkResult

  /// A reborrow operand.
  ///
  /// Call `visitUsesOfInner()` to recursively classify scope-ending
  /// uses (reborrow and end_borrow).
  mutating func reborrowingUse(of operand: Operand, isInnerLifetime: Bool)
    -> WalkResult
}

extension OwnershipUseVisitor {
  /// Classify a non-address type operand, dispatching to one of the
  /// protocol methods below.
  mutating func classify(operand: Operand) -> WalkResult {
    switch operand.value.ownership {
    case .owned:
      return classifyOwned(operand: operand)
    case .guaranteed:
      return classifyGuaranteed(operand: operand)
    case .none, .unowned:
      return .continueWalk
    }
  }

  /// Visit all uses that contribute to the ownership live
  /// range of `value`. This does not assume that `value` has a
  /// complete lifetime, and non-lifetime-ending uses are visited.
  ///
  /// If `value` is a phi (owned or reborrowed), then find its inner
  /// adjacent phis and treat them like inner borrows.
  ///
  /// This is only called for uses in the outer lifetime.
  mutating func visitUsesOfOuter(value: Value) -> WalkResult {
    switch value.ownership {
    case .owned:
      return value.uses.ignoreTypeDependence.walk { classifyOwned(operand: $0) }
    case .guaranteed:
      return value.uses.ignoreTypeDependence.walk {
        classifyGuaranteed(operand: $0) }
    case .none, .unowned:
      return .continueWalk
    }
  }

  /// Visit only those uses of a value within an inner borrow scope
  /// that may affect the outer lifetime. An inner borrow scope is one
  /// in which the borrowing operand is itself a use of the outer
  /// lifetime, including: begin_borrow, reborrow, partial_apply,
  /// mark_dependence, or an inner adjacent phi (where original SSA
  /// def is a phi in the same block).
  mutating func visitUsesOfInner(value: Value) -> WalkResult {
    if let beginBorrow = BeginBorrowValue(value) {
      return beginBorrow.scopeEndingOperands.walk {
        switch $0.ownership {
        case .endBorrow:
          return ownershipLeafUse(of: $0, isInnerLifetime: true)
        case .reborrow:
          return reborrowingUse(of: $0, isInnerLifetime: true)
        default:
          fatalError("invalid borrow scope ending operand ownership")
        }
      }
    }
    // When a borrow introduces an owned value, each OSSA lifetime is
    // effectively a separate borrow scope. A destroy ends the borrow
    // scope, while a forwarding consume effectively "reborrows".
    assert(value.ownership == .owned,
      "inner value must be a reborrow or owned forward")
    return value.uses.endingLifetime.walk {
      switch $0.ownership {
      case .forwardingConsume:
        return forwardingUse(of: $0, isInnerLifetime: true)
      case .destroyingConsume:
        return ownershipLeafUse(of: $0, isInnerLifetime: true)
      default:
        fatalError("invalid owned lifetime ending operand ownership")
      }
    }
  }

  // Visit uses of borrowing instruction (operandOwnerhip == .borrow),
  // skipping uses within the borrow scope.
  mutating func visitInnerBorrowUses(of borrowInst: BorrowingInstruction)
    -> WalkResult {
    // If a borrowed value is introduced, then handle the inner scope.
    if let beginBorrow = BeginBorrowValue(resultOf: borrowInst) {
      return visitUsesOfInner(value: beginBorrow.value)
    }
    // Otherwise, directly visit the scope ending uses.
    //
    // TODO: remove this stack by changign visitScopeEndingOperands to
    // take a non-escaping closure that can call ownershipLeafUse.
    var stack = Stack<Operand>(context)
    defer { stack.deinitialize() }
    _ = borrowInst.visitScopeEndingOperands(context) {
      stack.push($0)
      return .continueWalk
    }
    return stack.walk { ownershipLeafUse(of: $0, isInnerLifetime: true) }
  }
}

extension OwnershipUseVisitor {
  // This is only called for uses in the outer lifetime.
  private mutating func classifyOwned(operand: Operand) -> WalkResult {
    switch operand.ownership {
    case .nonUse:
      return .continueWalk

    case .destroyingConsume:
      return ownershipLeafUse(of: operand, isInnerLifetime: false)

    case .forwardingConsume:
      return forwardingUse(of: operand, isInnerLifetime: false)

    case .pointerEscape:
      return pointerEscapingUse(of: operand)

    case .instantaneousUse, .forwardingUnowned, .unownedInstantaneousUse,
      .bitwiseEscape:
      return ownershipLeafUse(of: operand, isInnerLifetime: false)

    case .borrow:
      return visitBorrowingUse(of: operand)

    // TODO: Eventually, visit owned InteriorPointers as implicit borrows.
    case .interiorPointer, .trivialUse, .endBorrow, .reborrow,
      .guaranteedForwarding:
      fatalError("ownership incompatible with an owned value");
    }
  }

  // This is only called for uses in the outer lifetime.
  private mutating func classifyGuaranteed(operand: Operand)
    -> WalkResult {
    switch operand.ownership {
    case .nonUse:
      return .continueWalk

    case .pointerEscape:
      // TODO: Change ProjectBox ownership to InteriorPointer and allow them to take owned values.
      if operand.instruction is ProjectBoxInst {
        return visitInteriorPointerUse(of: operand)
      }
      return pointerEscapingUse(of: operand)

    case .instantaneousUse, .forwardingUnowned, .unownedInstantaneousUse,
         .bitwiseEscape, .endBorrow:
      return ownershipLeafUse(of: operand, isInnerLifetime: false)

    case .reborrow:
      return reborrowingUse(of: operand, isInnerLifetime: false)

    case .guaranteedForwarding:
      return forwardingUse(of: operand, isInnerLifetime: false)

    case .borrow:
      return visitBorrowingUse(of: operand)

    case .interiorPointer:
      return visitInteriorPointerUse(of: operand)

    case .trivialUse, .forwardingConsume, .destroyingConsume:
      fatalError("ownership incompatible with a guaranteed value")
    }
  }

  private mutating func visitBorrowingUse(of operand: Operand)
    -> WalkResult {
    switch operand.instruction {
    case let pai as PartialApplyInst:
      assert(!pai.mayEscape)
      return dependentUse(of: operand, into: pai)
    case let mdi as MarkDependenceInst:
      assert(operand == mdi.baseOperand && mdi.isNonEscaping)
      return dependentUse(of: operand, into: mdi)
    default:
      return borrowingUse(of: operand,
                          by: BorrowingInstruction(operand.instruction)!)
    }
  }

  private mutating func visitInteriorPointerUse(of operand: Operand)
    -> WalkResult {
    switch operand.instruction {
    case is RefTailAddrInst, is RefElementAddrInst, is ProjectBoxInst,
         is OpenExistentialBoxInst:
      let svi = operand.instruction as! SingleValueInstruction
      return interiorPointerUse(of: operand, into: svi)
    default:
      return pointerEscapingUse(of: operand)
    }
  }
}

/// Visit all interior uses of an OSSA lifetime.
///
/// - `definingValue` dominates all uses. Only dominated phis extend
/// the lifetime. All other phis must have a lifetime-ending outer
/// adjacent phi; otherwise they will be recorded as `unenclosedPhis`.
///
/// - Does not assume the current lifetime is linear. Transitively
/// follows guaranteed forwarding and address uses within the current
/// scope. Phis that are not dominanted by definingValue or an outer
/// adjacent phi are marked "unenclosed" to signal an incomplete
/// lifetime.
///
/// - Assumes inner scopes *are* linear, including borrow and address
/// scopes (e.g. begin_borrow, load_borrow, begin_apply, store_borrow,
/// begin_access) A `innerScopeHandler` callback may be used to
/// complete inner scopes before updating liveness.
///
/// InteriorUseWalker can be used to complete (linearize) an OSSA
/// lifetime after transformation that invalidates OSSA.
///
/// Example:
///
///     %s = struct ...
///     %f = struct_extract %s     // defines a guaranteed value (%f)
///     %b = begin_borrow %f
///     %a = ref_element_addr %b
///     _  = address_to_pointer %a
///     end_borrow %b              // the only interior use of %f
///
/// When computing interior liveness for %f, %b is an inner
/// scope. Because inner scopes are complete, the only relevant use is
/// end_borrow %b. Despite the address_to_pointer instruction, %f does
/// not escape any dependent address. 
///
/// TODO: Implement the hasPointerEscape flags on BeginBorrowInst,
/// MoveValueInst, and Allocation. Then this visitor should assert
/// that the forward-extended lifetime introducer has no pointer
/// escaping uses.
struct InteriorUseWalker {
  let functionContext: FunctionPassContext
  var context: Context { functionContext }

  let definingValue: Value
  let ignoreEscape: Bool
  let useVisitor: (Operand) -> WalkResult

  var innerScopeHandler: InnerScopeHandler? = nil

  private func handleInner(borrowed value: Value) -> WalkResult  {
    guard let innerScopeHandler else {
      return .continueWalk
    }
    return innerScopeHandler(value)
  }

  var unenclosedPhis: [Phi] = []

  var function: Function { definingValue.parentFunction }

  var pointerStatus: InteriorPointerStatus = .nonescaping

  private var visited: ValueSet

  mutating func deinitialize() {
    visited.deinitialize()
  }

  init(definingValue: Value, ignoreEscape: Bool, _ context: FunctionPassContext,
    visitor: @escaping (Operand) -> WalkResult) {
    assert(!definingValue.type.isAddress, "address values have no ownership")
    self.functionContext = context
    self.definingValue = definingValue
    self.ignoreEscape = ignoreEscape
    self.useVisitor = visitor
    self.visited = ValueSet(context)
  }

  mutating func visitUses() -> WalkResult {
    // If the outer value is an owned phi or reborrow, consider inner
    // adjacent phis part of its lifetime.
    if let phi = Phi(definingValue), phi.endsLifetime {
      let result = phi.innerAdjacentPhis.walk { innerPhi in
        if innerPhi.isReborrow {
          // Inner adjacent reborrows are considered inner borrow scopes.
          if handleInner(borrowed: innerPhi.value) == .abortWalk {
            return .abortWalk
          }
          return visitUsesOfInner(value: innerPhi.value)
        } else {
          // Inner adjacent guaranteed phis are uses of the outer borrow.
          return visitUsesOfOuter(value: innerPhi.value)
        }
      }
      if result == .abortWalk {
        return .abortWalk
      }
    }
    return visitUsesOfOuter(value: definingValue)
  }
}

extension InteriorUseWalker: OwnershipUseVisitor {
  /// This is invoked for all non-address uses of the outer lifetime,
  /// even if the use forwards a value or produces an interior
  /// pointer. This is only invoked for uses of an inner lifetime
  /// if it ends the lifetime.
  mutating func ownershipLeafUse(of operand: Operand, isInnerLifetime: Bool)
    -> WalkResult {
    useVisitor(operand)
  }

  // Visit owned and guaranteed forwarding operands.
  //
  // Guaranteed forwarding operands extend the outer lifetime.
  //
  // Owned forwarding operands end the outer lifetime but extend the
  // inner lifetime (e.g. from a PartialApply or MarkDependence).
  mutating func forwardingUse(of operand: Operand, isInnerLifetime: Bool)
    -> WalkResult {
    switch operand.value.ownership {
    case .guaranteed:
      assert(!isInnerLifetime, "inner guaranteed forwards are not walked")
      return walkDown(operand: operand)
    case .owned:
      return isInnerLifetime ? walkDown(operand: operand) : useVisitor(operand)
    default:
      fatalError("forwarded values must have a lifetime")
    }
  }

  mutating func interiorPointerUse(of operand: Operand, into address: Value)
    -> WalkResult {
    // OSSA lifetime ignores trivial types.
    if operand.value.type.isTrivial(in: function) {
      return .continueWalk
    }
    if useVisitor(operand) == .abortWalk {
      return .abortWalk
    }
    return walkDownAddressUses(of: address)
  }  

  // Handle partial_apply [on_stack] and mark_dependence [nonescaping].
  //
  // TODO: Rather than walking down the owned uses, this could call
  // visitInnerBorrowUses, but we need to ensure all dependent values
  // are complete first:
  //
  //   if let svi = borrowInst as! SingleValueInstruction,
  //          svi.ownership == .owned {
  //     if handleInner(borrowed: beginBorrow.value) == .abortWalk {
  //       return .abortWalk
  //     }
  //     return visitInnerBorrowUses(of: borrowInst)
  //   }
  mutating func dependentUse(of operand: Operand, into value: Value)
    -> WalkResult {
    // OSSA lifetime ignores trivial types.
    if operand.value.type.isTrivial(in: function) {
      return .continueWalk
    }
    if useVisitor(operand) == .abortWalk {
      return .abortWalk
    }
    return walkDownUses(of: value)
  }  

  mutating func pointerEscapingUse(of operand: Operand) -> WalkResult {
    if useVisitor(operand) == .abortWalk {
      return .abortWalk
    }
    pointerStatus.setEscaping(operand: operand)
    return ignoreEscape ? .continueWalk : .abortWalk
  }

  // Call the innerScopeHandler before visiting the scope-ending uses.
  mutating func borrowingUse(of operand: Operand,
                             by borrowInst: BorrowingInstruction)
    -> WalkResult {
    if let beginBorrow = BeginBorrowValue(resultOf: borrowInst) {
      if handleInner(borrowed: beginBorrow.value) == .abortWalk {
        return .abortWalk
      }
    }
    return visitInnerBorrowUses(of: borrowInst)
  }

  // Visit a reborrow operand. This ends an outer lifetime and extends
  // an inner lifetime.
  mutating func reborrowingUse(of operand: Operand, isInnerLifetime: Bool)
    -> WalkResult {
    return isInnerLifetime ? walkDown(operand: operand) : useVisitor(operand)
  }
}

extension InteriorUseWalker: AddressUseVisitor {
  /// An address projection produces a single address result and does not
  /// escape its address operand in any other way.
  mutating func projectedAddressUse(of operand: Operand, into value: Value)
    -> WalkResult {
    return walkDownAddressUses(of: value)
  }

  mutating func appliedAddressUse(of operand: Operand, by apply: FullApplySite)
    -> WalkResult {
    if apply is BeginApplyInst {
      return scopedAddressUse(of: operand)
    }
    return leafAddressUse(of: operand)
  }

  mutating func scopedAddressUse(of operand: Operand) -> WalkResult {
    switch operand.instruction {
    case let ba as BeginAccessInst:
      if handleInner(borrowed: ba) == .abortWalk {
        return .abortWalk
      }
      return ba.endOperands.walk { useVisitor($0) }
    case let ba as BeginApplyInst:
      if handleInner(borrowed: ba.token) == .abortWalk {
        return .abortWalk
      }
      return ba.token.uses.walk {
        useVisitor($0)
      }
    case let sb as StoreBorrowInst:
      if handleInner(borrowed: sb) == .abortWalk {
        return .abortWalk
      }
      return sb.uses.filterUsers(ofType: EndBorrowInst.self).walk {
        useVisitor($0)
      }
    case let load as LoadBorrowInst:
      if handleInner(borrowed: load) == .abortWalk {
        return .abortWalk
      }
      return load.uses.endingLifetime.walk {
        useVisitor($0)
      }
    default:
      fatalError("Unrecognized scoped address use: \(operand.instruction)")
    }
  }

  mutating func scopeEndingAddressUse(of operand: Operand) -> WalkResult {
    return .continueWalk
  }

  mutating func leafAddressUse(of operand: Operand) -> WalkResult {
    return .continueWalk
  }

  mutating func loadedAddressUse(of operand: Operand, into value: Value)
    -> WalkResult {
    return .continueWalk
  }    

  mutating func loadedAddressUse(of operand: Operand, into address: Operand)
    -> WalkResult {
    return .continueWalk
  }    

  mutating func dependentAddressUse(of operand: Operand, into value: Value)
    -> WalkResult {
    walkDownUses(of: value)
  }    

  mutating func escapingAddressUse(of operand: Operand) -> WalkResult {
    pointerStatus.setEscaping(operand: operand)
    return ignoreEscape ? .continueWalk : .abortWalk
  }

  mutating func unknownAddressUse(of operand: Operand) -> WalkResult {
    pointerStatus = .unknown(operand)
    return .abortWalk
  }

  private mutating func walkDownAddressUses(of address: Value) -> WalkResult {
    assert(address.type.isAddress)
    return address.uses.ignoreTypeDependence.walk {
      // Record all uses
      if useVisitor($0) == .abortWalk {
        return .abortWalk
      }
      return classifyAddress(operand: $0)
    }
  }
}

// Helpers to walk down forwarding operations.
extension InteriorUseWalker {
  // Walk down forwarding operands
  private mutating func walkDown(operand: Operand) -> WalkResult {
    // Record all uses
    if useVisitor(operand) == .abortWalk {
      return .abortWalk
    }
    if let inst = operand.instruction as? ForwardingInstruction {
      return inst.forwardedResults.walk { walkDownUses(of: $0) }
    }
    if let phi = Phi(using: operand) {
      if phi.value.ownership == .guaranteed {
        return walkDown(guaranteedPhi: phi)
      }
      // This is a phi of a dependent value. partial_apply [on_stack]
      // and mark_dependence [nonescaping] cannot be cloned, so all
      // dependent phis must be dominated.
      assert(definingValue.parentBlock.dominates(phi.successor,
                                                 functionContext.dominatorTree),
             "on-stack partial apply cannot be cloned")
      return walkDownUses(of: phi.value)
    }
    // TODO: verify that ForwardInstruction handles all .forward
    // operand ownership and change this to a fatalError.
    return .continueWalk
  }

  private mutating func walkDownUses(of value: Value) -> WalkResult {
    guard value.ownership.hasLifetime else {
      return .continueWalk
    }
    guard visited.insert(value) else {
      return .continueWalk
    }
    switch value.ownership {
    case .owned:
      return visitUsesOfInner(value: value)
    case .guaranteed:
      return visitUsesOfOuter(value: value)
    default:
      fatalError("ownership requires a lifetime")
    }
  }

  // Dominating definingValue example: walkDown must continue visiting
  // uses of a reborrow in the inner borrow scope:
  //
  // bb0:
  //  d1 = ...
  //  cond_br bb1, bb2
  // bb1:
  //   b1 = borrow d1
  //   br bb3(b1)
  // bb2:
  //   b2 = borrow d1
  //   br bb3(b2)
  // bb3(reborrow):
  //   u1 = d1
  //   u2 = reborrow
  //   // can't move destroy above u2
  //   destroy_value d1
  //
  // Dominating definingValue example: walkDown must continue visiting
  // uses of a guaranteed phi in the outer lifetime:
  //
  // bb0:
  //  b1 = borrow d1
  //  cond_br bb1, bb2
  // bb1:
  //   p1 = projection b1
  //   br bb3(p1)
  // bb2:
  //   p1 = projection b1
  //   br bb3(p2)
  // bb3(forwardingPhi):
  //   u1 = b1
  //   u2 = forwardingPhi
  //   // can't move end_borrow above u2
  //   end_borrow b1
  private mutating func walkDown(guaranteedPhi: Phi) -> WalkResult {
    guard visited.insert(guaranteedPhi.value) else {
      return .continueWalk
    }
    let phiValue = guaranteedPhi.value.lookThroughBorrowedFromUser
    guard phiValue.getEnclosingValues(functionContext).contains(definingValue) else {
      // Since definingValue is not an enclosing value, it must be
      // consumed or reborrowed by some outer adjacent phi in this
      // block. An outer adjacent phi's uses do not contribute to the
      // outer liveness. Instead, guaranteedPhi will be recorded as a
      // regular lifetime-ending use by the visitor.
      return .continueWalk
    }
    // definingValue is not consumed or reborrowed by an outer
    // adjacent phi in guaranteedPhi's block. Therefore this
    // guaranteedPhi's uses contribute to the liveness of
    // definingValue.
    //
    // TODO: instead of relying on Dominance, we can reformulate
    // this algorithm to detect redundant phis, similar to the
    // SSAUpdater.
    if !definingValue.parentBlock.dominates(guaranteedPhi.successor,
      functionContext.dominatorTree) {
      // definingValue does not dominate guaranteedPhi. Record this
      // unenclosed phi so the liveness client can insert the missing
      // outer adjacent phi.
      unenclosedPhis.append(guaranteedPhi);
      return .continueWalk
    }
    // Since definingValue dominates guaranteedPhi, this is a well-formed linear
    // lifetime, and liveness can proceed.
    if guaranteedPhi.isReborrow {
      return visitUsesOfInner(value: guaranteedPhi.value)
    } else {
      return visitUsesOfOuter(value: guaranteedPhi.value)
    }
  }
}

/// Cache the liveness boundary by taking a snapshot of its InstructionRange.
struct LivenessBoundary : CustomStringConvertible {
  var lastUsers : Stack<Instruction>
  var boundaryEdges : Stack<BasicBlock>
  var deadDefs : Stack<Value>

  // Compute the boundary of a singly-defined range.
  init(value: Value, range: InstructionRange, _ context: Context) {
    assert(range.isValid)

    lastUsers = Stack<Instruction>(context)
    boundaryEdges = Stack<BasicBlock>(context)
    deadDefs = Stack<Value>(context)

    lastUsers.append(contentsOf: range.ends)
    boundaryEdges.append(contentsOf: range.exitBlocks)
    if lastUsers.isEmpty {
      deadDefs.push(value)
      assert(boundaryEdges.isEmpty)
    }
  }

  var description: String {
    (lastUsers.map { "last user: \($0.description)" }
    + boundaryEdges.map { "boundary edge: \($0.description)" }
    + deadDefs.map { "dead def: \($0.description)" }).joined(separator: "\n")
  }

  mutating func deinitialize() {
    lastUsers.deinitialize()
    boundaryEdges.deinitialize()
    deadDefs.deinitialize()
  }
}

let linearLivenessTest = FunctionTest("linear_liveness_swift") {
  function, arguments, context in
  let value = arguments.takeValue()
  print("Linear liveness: \(value)")
  var range = computeLinearLiveness(for: value, context)
  defer { range.deinitialize() }
  print("Live blocks:")
  print(range)
  var boundary = LivenessBoundary(value: value, range: range, context)
  defer { boundary.deinitialize() }
  print(boundary)
}

let interiorLivenessTest = FunctionTest("interior_liveness_swift") {
  function, arguments, context in
  let value = arguments.takeValue()
  print("Interior liveness: \(value)")

  var range = InstructionRange(for: value, context)
  defer { range.deinitialize() }

  var visitor = InteriorUseWalker(definingValue: value, ignoreEscape: true, context) {
    range.insert($0.instruction)
    return .continueWalk
  }
  defer { visitor.deinitialize() }

  let success = visitor.visitUses()

  switch visitor.pointerStatus {
  case .nonescaping:
    break
  case let .escaping(operands):
    for operand in operands {
      print("Pointer escape: \(operand.instruction)")
    }
  case let .unknown(operand):
    print("Unrecognized SIL address user \(operand.instruction)")
  }
  if success == .abortWalk {
    print("Incomplete liveness")
  }
  print(range)
  print("Unenclosed phis {")
  visitor.unenclosedPhis.forEach { print("  \($0)") } 
  print("}")

  var boundary = LivenessBoundary(value: value, range: range, context)
  defer { boundary.deinitialize() }
  print(boundary)
}
