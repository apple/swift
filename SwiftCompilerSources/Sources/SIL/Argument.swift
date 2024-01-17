//===--- Argument.swift - Defines the Argument classes --------------===//
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

import Basic
import SILBridging

/// A basic block argument.
///
/// Maps to both, SILPhiArgument and SILFunctionArgument.
public class Argument : Value, Hashable {
  public var definingInstruction: Instruction? { nil }

  public var parentBlock: BasicBlock {
    return bridged.getParent().block
  }

  var bridged: BridgedArgument { BridgedArgument(obj: SwiftObject(self)) }
  
  public var index: Int {
    return parentBlock.arguments.firstIndex(of: self)!
  }

  public var isReborrow: Bool { bridged.isReborrow() }

  public var varDecl: VarDecl? { VarDecl(bridged: bridged.getVarDecl()) }

  public var sourceLoc: SourceLoc? { varDecl?.sourceLoc }

  public static func ==(lhs: Argument, rhs: Argument) -> Bool {
    lhs === rhs
  }

  public func hash(into hasher: inout Hasher) {
    hasher.combine(ObjectIdentifier(self))
  }
}

final public class FunctionArgument : Argument {
  public var convention: ArgumentConvention {
    parentFunction.argumentConventions[index]
  }

  public var isSelf: Bool {
    parentFunction.argumentConventions.selfIndex == index
  }

  // FIXME: This is incorrect in two cases: it does not include the
  // indirect error result, and, prior to address lowering, does not
  // include pack results.
  public var isIndirectResult: Bool {
    return index < parentFunction.numIndirectResultArguments
  }

  public var hasResultDependsOn : Bool {
    return bridged.hasResultDependsOn()
  }

  /// If the function's result depends on this argument, return the
  /// kind of dependence.
  public var resultDependence: LifetimeDependenceConvention? {
    parentFunction.argumentConventions[resultDependsOn: index]
  }
}

public struct Phi {
  public let value: Argument

  // TODO: Remove the CondBr case. All passes avoid critical edges. It
  // is only included here for compatibility with .sil tests that have
  // not been migrated.
  public init?(_ value: Value) {
    guard let argument = value as? Argument else { return nil }
    var preds = argument.parentBlock.predecessors
    guard let pred = preds.next() else { return nil }
    let term = pred.terminator
    guard term is BranchInst || term is CondBranchInst else { return nil }
    self.value = argument
  }

  public init?(using operand: Operand) {
    switch operand.instruction {
    case let br as BranchInst:
      self.init(br.getArgument(for: operand))
    case let condBr as CondBranchInst:
      guard let arg = condBr.getArgument(for: operand) else { return nil }
      self.init(arg)
    default:
      return nil
    }
  }

  public var predecessors: PredecessorList {
    return value.parentBlock.predecessors
  }

  public var successor: BasicBlock {
    return value.parentBlock
  }

  public func incomingOperand(inPredecessor predecessor: BasicBlock)
  -> Operand {
    let blockArgIdx = value.index
    switch predecessor.terminator {
    case let br as BranchInst:
      return br.operands[blockArgIdx]
    case let condBr as CondBranchInst:
      if condBr.trueBlock == successor {
        assert(condBr.falseBlock != successor)
        return condBr.trueOperands[blockArgIdx]
      } else {
        assert(condBr.falseBlock == successor)
        return condBr.falseOperands[blockArgIdx]
      }
    default:
      fatalError("wrong terminator for phi-argument")
    }
  }

  public var incomingOperands: LazyMapSequence<PredecessorList, Operand> {
    predecessors.lazy.map { incomingOperand(inPredecessor: $0) }
  }

  public var incomingValues: LazyMapSequence<LazyMapSequence<PredecessorList, Operand>, Value> {
    incomingOperands.lazy.map { $0.value }
  }

  public var isReborrow: Bool { value.isReborrow }

  public var endsLifetime: Bool {
    value.ownership == .owned || value.isReborrow
  }

  public var borrowedFrom: BorrowedFromInst? {
    for use in value.uses {
      if let bfi = use.forwardingBorrowedFromUser {
        return bfi
      }
    }
    return nil
  }

  public static func ==(lhs: Phi, rhs: Phi) -> Bool {
    lhs.value === rhs.value
  }

  public func hash(into hasher: inout Hasher) {
    value.hash(into: &hasher)
  }
}

extension Operand {
  public var forwardingBorrowedFromUser: BorrowedFromInst? {
    if let bfi = instruction as? BorrowedFromInst, index == 0 {
      return bfi
    }
    return nil
  }
}

public struct TerminatorResult {
  public let value: Argument

  public init?(_ value: Value) {
    guard let argument = value as? Argument else { return nil }
    var preds = argument.parentBlock.predecessors
    guard let pred = preds.next() else { return nil }
    let term = pred.terminator
    if term is BranchInst || term is CondBranchInst { return nil }
    self.value = argument
  }

  public var terminator: TermInst {
    var preds = value.parentBlock.predecessors
    return preds.next()!.terminator
  }
  
  public var predecessor: BasicBlock {
    return terminator.parentBlock
  }

  public var successor: BasicBlock {
    return value.parentBlock
  }

  public static func ==(lhs: TerminatorResult, rhs: TerminatorResult) -> Bool {
    lhs.value == rhs.value
  }

  public func hash(into hasher: inout Hasher) {
    value.hash(into: &hasher)
  }
}

/// ArgumentConventions indexed on a SIL function's argument index.
/// When derived from an ApplySite, this corresponds to the callee
/// function's argument index.
///
/// When derived from an ApplySite, `convention` is the substituted
/// convention. Substitution only affects the type inside ResultInfo
/// and ParameterInfo. It does not change the resulting
/// ArgumentConvention, ResultConvention, or LifetimeDependenceInfo.
public struct ArgumentConventions : Collection, CustomStringConvertible {
  public let convention: FunctionConvention

  public var startIndex: Int { 0 }

  public var endIndex: Int {
    firstParameterIndex + convention.parameters.count
  }

  public func index(after index: Int) -> Int {
    return index + 1
  }

  public subscript(_ argumentIndex: Int) -> ArgumentConvention {
    if let paramIdx = parameterIndex(for: argumentIndex) {
      return convention.parameters[paramIdx].convention
    }
    let resultInfo = convention.indirectSILResults[argumentIndex]
    return ArgumentConvention(result: resultInfo.convention)
  }

  public subscript(result argumentIndex: Int) -> ResultInfo? {
    if parameterIndex(for: argumentIndex) != nil {
      return nil
    }
    return convention.indirectSILResults[argumentIndex]
  }

  public subscript(parameter argumentIndex: Int) -> ParameterInfo? {
    guard let paramIdx = parameterIndex(for: argumentIndex) else {
      return nil
    }
    return convention.parameters[paramIdx]
  }

  /// Return a dependence of the function results on the indexed parameter.
  public subscript(resultDependsOn argumentIndex: Int)
    -> LifetimeDependenceConvention? {
    guard let paramIdx = parameterIndex(for: argumentIndex) else {
      return nil
    }
    return convention.resultDependencies?[paramIdx]
  }

  /// Number of SIL arguments for the function type's results
  /// including the error result. Use this to avoid lazy iteration
  /// over indirectSILResults to find the count.
  var indirectSILResultCount: Int {
    convention.indirectSILResultCount
  }

  /// The SIL argument index of the function type's first parameter.
  public var firstParameterIndex: Int { indirectSILResultCount }

  /// The SIL argument index of the 'self' parameter.
  var selfIndex: Int? {
    guard convention.hasSelfParameter else { return nil }
    // self is the last parameter
    return endIndex - 1
  }

  public var description: String {
    let origTy = convention.bridgedFunctionType
    var str = String(taking: origTy.getDebugDescription())
    for idx in startIndex..<indirectSILResultCount {
      str += "\n[\(idx)]  indirect result: " + self[idx].description
    }
    for idx in indirectSILResultCount..<endIndex {
      str += "\n[\(idx)]        parameter: " + self[idx].description
      if let dep = self[resultDependsOn: idx] {
        str += "resultDependsOn: " + dep.description
      }
    }
    return str
  }
}

extension ArgumentConventions {
  private func parameterIndex(for argIdx: Int) -> Int? {
    let firstParamIdx = firstParameterIndex  // bridging call
    return argIdx < firstParamIdx ? nil : argIdx - firstParamIdx
  }
}

public struct YieldConventions : Collection, CustomStringConvertible {
  public let convention: FunctionConvention

  public var yields: FunctionConvention.Yields {
    return convention.yields
  }

  public var startIndex: Int { 0 }

  public var endIndex: Int { yields.count }

  public func index(after index: Int) -> Int {
    return index + 1
  }

  public subscript(_ index: Int) -> ArgumentConvention {
    return yields[index].convention
  }

  public var description: String {
    var str = String(
      taking: convention.bridgedFunctionType.getDebugDescription())
    yields.forEach {
      str += "\n      yield: " + $0.description
    }
    return str
  }
}

public enum ArgumentConvention : CustomStringConvertible {
  /// This argument is passed indirectly, i.e. by directly passing the address
  /// of an object in memory.  The callee is responsible for destroying the
  /// object.  The callee may assume that the address does not alias any valid
  /// object.
  case indirectIn

  /// This argument is passed indirectly, i.e. by directly passing the address
  /// of an object in memory.  The callee may not modify and does not destroy
  /// the object.
  case indirectInGuaranteed

  /// This argument is passed indirectly, i.e. by directly passing the address
  /// of an object in memory.  The object is always valid, but the callee may
  /// assume that the address does not alias any valid object and reorder loads
  /// stores to the parameter as long as the whole object remains valid. Invalid
  /// single-threaded aliasing may produce inconsistent results, but should
  /// remain memory safe.
  case indirectInout

  /// This argument is passed indirectly, i.e. by directly passing the address
  /// of an object in memory. The object is allowed to be aliased by other
  /// well-typed references, but is not allowed to be escaped. This is the
  /// convention used by mutable captures in @noescape closures.
  case indirectInoutAliasable

  /// This argument represents an indirect return value address. The callee stores
  /// the returned value to this argument. At the time when the function is called,
  /// the memory location referenced by the argument is uninitialized.
  case indirectOut

  /// This argument is passed directly.  Its type is non-trivial, and the callee
  /// is responsible for destroying it.
  case directOwned

  /// This argument is passed directly.  Its type may be trivial, or it may
  /// simply be that the callee is not responsible for destroying it. Its
  /// validity is guaranteed only at the instant the call begins.
  case directUnowned

  /// This argument is passed directly.  Its type is non-trivial, and the caller
  /// guarantees its validity for the entirety of the call.
  case directGuaranteed

  /// This argument is a value pack of mutable references to storage,
  /// which the function is being given exclusive access to.  The elements
  /// must be passed indirectly.
  case packInout

  /// This argument is a value pack, and ownership of the elements is being
  /// transferred into this function.  Whether the elements are passed
  /// indirectly is recorded in the pack type.
  case packOwned

  /// This argument is a value pack, and ownership of the elements is not
  /// being transferred into this function.  Whether the elements are passed
  /// indirectly is recorded in the pack type.
  case packGuaranteed

  /// This argument is a pack of indirect return value addresses.  The
  /// addresses are stored in the pack by the caller and read out by the
  /// callee; within the callee, they are individually treated like
  /// indirectOut arguments.
  case packOut

  public init(result: ResultConvention) {
    switch result {
    case .indirect:
      self = .indirectOut
    case .owned:
      self = .directOwned
    case .unowned, .unownedInnerPointer, .autoreleased:
      self = .directUnowned
    case .pack:
      self = .packOut
    }
  }

  public var isIndirect: Bool {
    switch self {
    case .indirectIn, .indirectInGuaranteed,
         .indirectInout, .indirectInoutAliasable, .indirectOut,
         .packOut, .packInout, .packOwned, .packGuaranteed:
      return true
    case .directOwned, .directUnowned, .directGuaranteed:
      return false
    }
  }

  public var isIndirectIn: Bool {
    switch self {
    case .indirectIn, .indirectInGuaranteed,
         .packOwned, .packGuaranteed:
      return true
    case .directOwned, .directUnowned, .directGuaranteed,
         .indirectInout, .indirectInoutAliasable, .indirectOut,
         .packOut, .packInout:
      return false
    }
  }

  public var isIndirectOut: Bool {
    switch self {
    case .indirectOut, .packOut:
      return true
    case .indirectInGuaranteed, .directGuaranteed, .packGuaranteed,
         .indirectIn, .directOwned, .directUnowned,
         .indirectInout, .indirectInoutAliasable,
         .packInout, .packOwned:
      return false
    }
  }

  public var isGuaranteed: Bool {
    switch self {
    case .indirectInGuaranteed, .directGuaranteed, .packGuaranteed:
      return true
    case .indirectIn, .directOwned, .directUnowned,
         .indirectInout, .indirectInoutAliasable, .indirectOut,
         .packOut, .packInout, .packOwned:
      return false
    }
  }

  public var isExclusiveIndirect: Bool {
    switch self {
    case .indirectIn,
         .indirectOut,
         .indirectInGuaranteed,
         .indirectInout,
         .packOut,
         .packInout,
         .packOwned,
         .packGuaranteed:
      return true

    case .indirectInoutAliasable,
         .directUnowned,
         .directGuaranteed,
         .directOwned:
      return false
    }
  }

  public var isInout: Bool {
    switch self {
    case .indirectInout,
         .indirectInoutAliasable,
         .packInout:
      return true

    case .indirectIn,
         .indirectOut,
         .indirectInGuaranteed,
         .directUnowned,
         .directGuaranteed,
         .directOwned,
         .packOut,
         .packOwned,
         .packGuaranteed:
      return false
    }
  }

  public var description: String {
    switch self {
    case .indirectIn:
      return "indirectIn"
    case .indirectInGuaranteed:
      return "indirectInGuaranteed"
    case .indirectInout:
      return "indirectInout"
    case .indirectInoutAliasable:
      return "indirectInoutAliasable"
    case .indirectOut:
      return "indirectOut"
    case .directOwned:
      return "directOwned"
    case .directUnowned:
      return "directUnowned"
    case .directGuaranteed:
      return "directGuaranteed"
    case .packInout:
      return "packInout"
    case .packOwned:
      return "packOwned"
    case .packGuaranteed:
      return "packGuaranteed"
    case .packOut:
      return "packOut"
    }
  }
}

// Bridging utilities

extension BridgedArgument {
  public var argument: Argument { obj.getAs(Argument.self) }
  public var functionArgument: FunctionArgument { obj.getAs(FunctionArgument.self) }
}

extension BridgedArgumentConvention {
  var convention: ArgumentConvention {
    switch self {
      case .Indirect_In:             return .indirectIn
      case .Indirect_In_Guaranteed:  return .indirectInGuaranteed
      case .Indirect_Inout:          return .indirectInout
      case .Indirect_InoutAliasable: return .indirectInoutAliasable
      case .Indirect_Out:            return .indirectOut
      case .Direct_Owned:            return .directOwned
      case .Direct_Unowned:          return .directUnowned
      case .Direct_Guaranteed:       return .directGuaranteed
      case .Pack_Out:                return .packOut
      case .Pack_Inout:              return .packInout
      case .Pack_Owned:              return .packOwned
      case .Pack_Guaranteed:         return .packGuaranteed
      default:
        fatalError("unsupported argument convention")
    }
  }
}

extension ArgumentConvention {
  var bridged: BridgedArgumentConvention {
    switch self {
      case .indirectIn:             return .Indirect_In
      case .indirectInGuaranteed:   return .Indirect_In_Guaranteed
      case .indirectInout:          return .Indirect_Inout
      case .indirectInoutAliasable: return .Indirect_InoutAliasable
      case .indirectOut:            return .Indirect_Out
      case .directOwned:            return .Direct_Owned
      case .directUnowned:          return .Direct_Unowned
      case .directGuaranteed:       return .Direct_Guaranteed
      case .packOut:                return .Pack_Out
      case .packInout:              return .Pack_Inout
      case .packOwned:              return .Pack_Owned
      case .packGuaranteed:         return .Pack_Guaranteed
    }
  }
}
