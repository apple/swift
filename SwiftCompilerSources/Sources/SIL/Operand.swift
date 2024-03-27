//===--- Operand.swift - Instruction operands -----------------------------===//
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

/// An operand of an instruction.
public struct Operand : CustomStringConvertible, NoReflectionChildren {
  public let bridged: BridgedOperand

  public init(bridged: BridgedOperand) {
    self.bridged = bridged
  }

  init?(bridged: OptionalBridgedOperand) {
    guard let op = bridged.op else { return nil }
    self.bridged = BridgedOperand(op: op)
  }

  public var value: Value { bridged.getValue().value }

  public static func ==(lhs: Operand, rhs: Operand) -> Bool {
    return lhs.bridged.op == rhs.bridged.op
  }

  public var instruction: Instruction {
    return bridged.getUser().instruction
  }
  
  public var index: Int { instruction.operands.getIndex(of: self) }
  
  /// True if the operand is used to describe a type dependency, but it's not
  /// used as value.
  public var isTypeDependent: Bool { bridged.isTypeDependent() }

  public var endsLifetime: Bool { bridged.isLifetimeEnding() }

  public func canAccept(ownership: Ownership) -> Bool { bridged.canAcceptOwnership(ownership._bridged) }

  public var description: String { "operand #\(index) of \(instruction)" }
}

public struct OperandArray : RandomAccessCollection, CustomReflectable {
  private let base: OptionalBridgedOperand
  public let count: Int
  
  init(base: OptionalBridgedOperand, count: Int) {
    self.base = base
    self.count = count
  }

  init(base: Operand, count: Int) {
    self.base = OptionalBridgedOperand(bridged: base.bridged)
    self.count = count
  }

  public var startIndex: Int { return 0 }
  public var endIndex: Int { return count }
  
  public subscript(_ index: Int) -> Operand {
    assert(index >= startIndex && index < endIndex)
    return Operand(bridged: base.advancedBy(index))
  }
  
  public func getIndex(of operand: Operand) -> Int {
    let idx = base.distanceTo(operand.bridged)
    assert(self[idx].bridged.op == operand.bridged.op)
    return idx
  }
  
  public var customMirror: Mirror {
    let c: [Mirror.Child] = map { (label: nil, value: $0.value) }
    return Mirror(self, children: c)
  }
  
  /// Returns a sub-array defined by `bounds`.
  ///
  /// Note: this does not return a Slice. The first index of the returnd array is always 0.
  public subscript(bounds: Range<Int>) -> OperandArray {
    assert(bounds.lowerBound >= startIndex && bounds.upperBound <= endIndex)
    return OperandArray(
      base: OptionalBridgedOperand(op: base.advancedBy(bounds.lowerBound).op),
      count: bounds.upperBound - bounds.lowerBound)
  }

  public var values: LazyMapSequence<LazySequence<OperandArray>.Elements, Value> {
    self.lazy.map { $0.value }
  }
}

public struct UseList : CollectionLikeSequence {
  public struct Iterator : IteratorProtocol {
    var currentOpPtr: OptionalBridgedOperand
    
    public mutating func next() -> Operand? {
      if let op = currentOpPtr.operand {
        currentOpPtr = op.getNextUse()
        return Operand(bridged: op)
      }
      return nil
    }
  }

  private let firstOpPtr: OptionalBridgedOperand

  init(_ firstOpPtr: OptionalBridgedOperand) {
    self.firstOpPtr = firstOpPtr
  }

  public func makeIterator() -> Iterator {
    return Iterator(currentOpPtr: firstOpPtr)
  }
}

extension Sequence where Element == Operand {
  public var singleUse: Operand? {
    var result: Operand? = nil
    for op in self {
      if result != nil {
        return nil
      }
      result = op
    }
    return result
  }

  public var isSingleUse: Bool { singleUse != nil }

  public var ignoreTypeDependence: LazyFilterSequence<Self> {
    self.lazy.filter({!$0.isTypeDependent})
  }

  public var ignoreDebugUses: LazyFilterSequence<Self> {
    self.lazy.filter { !($0.instruction is DebugValueInst) }
  }

  public func filterUsers<I: Instruction>(ofType: I.Type) -> LazyFilterSequence<Self> {
    self.lazy.filter { $0.instruction is I }
  }

  public func ignoreUsers<I: Instruction>(ofType: I.Type) -> LazyFilterSequence<Self> {
    self.lazy.filter { !($0.instruction is I) }
  }

  public func ignore(user: Instruction) -> LazyFilterSequence<Self> {
    self.lazy.filter { !($0.instruction == user) }
  }

  public func getSingleUser<I: Instruction>(ofType: I.Type) -> I? {
    filterUsers(ofType: I.self).singleUse?.instruction as? I
  }

  public func getSingleUser<I: Instruction>(notOfType: I.Type) -> Instruction? {
    ignoreUsers(ofType: I.self).singleUse?.instruction
  }

  public var endingLifetime: LazyFilterSequence<Self> {
    return self.lazy.filter { $0.endsLifetime }
  }
}

extension Operand {
  /// Return true if this operation will store a full value into this
  /// operand's address.
  public var isAddressInitialization: Bool {
    if !value.type.isAddress {
      return false
    }
    switch instruction {
    case is StoringInstruction:
      return true
    case let srcDestInst as SourceDestAddrInstruction
           where srcDestInst.destinationOperand == self:
      return true
    case let apply as FullApplySite:
      return apply.isIndirectResult(operand: self)
    default:
      return false
    }
  }
}

extension OptionalBridgedOperand {
  init(bridged: BridgedOperand?) {
    self = OptionalBridgedOperand(op: bridged?.op)
  }
  var operand: BridgedOperand? {
    if let op = op {
      return BridgedOperand(op: op)
    }
    return nil
  }
}

/// Categorize all uses in terms of their ownership effect. Implies ownership and lifetime constraints.
public enum OperandOwnership {
  /// Operands that do not use the value. They only represent a dependence on a dominating definition and do not require liveness. (type-dependent operands)
  case nonUse
  
  /// Uses that can only handle trivial values. The operand value must have None ownership. These uses require liveness but are otherwise unverified.
  case trivialUse
  
  /// Use the value only for the duration of the operation, which may have side effects. (single-instruction apply with @guaranteed argument)
  case instantaneousUse
  
  /// Use a value without requiring or propagating ownership. The operation may not have side-effects that could affect ownership. This is limited to a small number of operations that are allowed to take Unowned values. (copy_value, single-instruction apply with @unowned argument))
  case unownedInstantaneousUse
  
  /// Forwarding instruction with an Unowned result. Its operands may have any ownership.
  case forwardingUnowned
  
  /// Escape a pointer into a value which cannot be tracked or verified.
  ///
  /// PointerEscape  operands indicate a SIL deficiency to suffuciently model dependencies. They never arise from user-level escapes.
  case pointerEscape
  
  /// Bitwise escape. Escapes the nontrivial contents of the value. OSSA does not enforce the lifetime of the escaping bits. The programmer must explicitly force lifetime extension. (ref_to_unowned, unchecked_trivial_bitcast)
  case bitwiseEscape
  
  /// Borrow. Propagates the owned or guaranteed value within a scope, without ending its lifetime. (begin_borrow, begin_apply with @guaranteed argument)
  case borrow
  
  /// Destroying Consume. Destroys the owned value immediately. (store, destroy, @owned destructure).
  case destroyingConsume

  /// Forwarding Consume. Consumes the owned value indirectly via a move. (br, destructure, tuple, struct, cast, switch).
  case forwardingConsume
  
  /// Interior Pointer. Propagates a trivial value (e.g. address, pointer, or no-escape closure) that depends on the guaranteed value within the base's borrow scope. The verifier checks that all uses of the trivial
  /// value are in scope. (ref_element_addr, open_existential_box)
  case interiorPointer
  
  /// Forwarded Borrow. Propagates the guaranteed value within the base's borrow scope. (tuple_extract, struct_extract, cast, switch)
  case guaranteedForwarding
  
  /// End Borrow. End the borrow scope opened directly by the operand. The operand must be a begin_borrow, begin_apply, or function argument. (end_borrow, end_apply)
  case endBorrow
  
  /// Reborrow. Ends the borrow scope opened directly by the operand and begins one or multiple disjoint borrow scopes. If a forwarded value is reborrowed, then its base must also be reborrowed at the same point. (br, FIXME: should also include destructure, tuple, struct)
  case reborrow

  public var endsLifetime: Bool {
    switch self {
    case .nonUse, .trivialUse, .instantaneousUse, .unownedInstantaneousUse,
         .forwardingUnowned, .pointerEscape, .bitwiseEscape, .borrow,
         .interiorPointer, .guaranteedForwarding:
      return false
    case .destroyingConsume, .forwardingConsume, .endBorrow, .reborrow:
      return true
    }
  }

  public var _bridged: BridgedOperand.OperandOwnership {
    switch self {
    case .nonUse:
      return BridgedOperand.OperandOwnership.NonUse
    case .trivialUse:
      return BridgedOperand.OperandOwnership.TrivialUse
    case .instantaneousUse:
      return BridgedOperand.OperandOwnership.InstantaneousUse
    case .unownedInstantaneousUse:
      return BridgedOperand.OperandOwnership.UnownedInstantaneousUse
    case .forwardingUnowned:
      return BridgedOperand.OperandOwnership.ForwardingUnowned
    case .pointerEscape:
      return BridgedOperand.OperandOwnership.PointerEscape
    case .bitwiseEscape:
      return BridgedOperand.OperandOwnership.BitwiseEscape
    case .borrow:
      return BridgedOperand.OperandOwnership.Borrow
    case .destroyingConsume:
      return BridgedOperand.OperandOwnership.DestroyingConsume
    case .forwardingConsume:
      return BridgedOperand.OperandOwnership.ForwardingConsume
    case .interiorPointer:
      return BridgedOperand.OperandOwnership.InteriorPointer
    case .guaranteedForwarding:
      return BridgedOperand.OperandOwnership.GuaranteedForwarding
    case .endBorrow:
      return BridgedOperand.OperandOwnership.EndBorrow
    case .reborrow:
      return BridgedOperand.OperandOwnership.Reborrow
    }
  }
}

extension Operand {
  public var ownership: OperandOwnership {
    switch bridged.getOperandOwnership() {
    case .NonUse: return .nonUse
    case .TrivialUse: return .trivialUse
    case .InstantaneousUse: return .instantaneousUse
    case .UnownedInstantaneousUse: return .unownedInstantaneousUse
    case .ForwardingUnowned: return .forwardingUnowned
    case .PointerEscape: return .pointerEscape
    case .BitwiseEscape: return .bitwiseEscape
    case .Borrow: return .borrow
    case .DestroyingConsume: return .destroyingConsume
    case .ForwardingConsume: return .forwardingConsume
    case .InteriorPointer: return .interiorPointer
    case .GuaranteedForwarding: return .guaranteedForwarding
    case .EndBorrow: return .endBorrow
    case .Reborrow: return .reborrow
    default:
      fatalError("unsupported operand ownership")
    }
  }
}
