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
  
  public static func ==(lhs: Argument, rhs: Argument) -> Bool {
    lhs === rhs
  }

  public func hash(into hasher: inout Hasher) {
    hasher.combine(ObjectIdentifier(self))
  }
}

final public class FunctionArgument : Argument {
  public var convention: ArgumentConvention {
    bridged.getConvention().convention
  }

  public var isIndirectResult: Bool {
    return index < parentFunction.numIndirectResultArguments
  }
}

final public class BlockArgument : Argument {
  public var isPhiArgument: Bool {
    parentBlock.predecessors.allSatisfy {
      let term = $0.terminator
      return term is BranchInst || term is CondBranchInst
    }
  }

  public var incomingPhiOperands: LazyMapSequence<PredecessorList, Operand> {
    assert(isPhiArgument)
    let idx = index
    return parentBlock.predecessors.lazy.map {
      switch $0.terminator {
        case let br as BranchInst:
          return br.operands[idx]
        case let condBr as CondBranchInst:
          if condBr.trueBlock == self.parentBlock {
            assert(condBr.falseBlock != self.parentBlock)
            return condBr.trueOperands[idx]
          } else {
            assert(condBr.falseBlock == self.parentBlock)
            return condBr.falseOperands[idx]
          }
        default:
          fatalError("wrong terminator for phi-argument")
      }
    }
  }

  public var incomingPhiValues: LazyMapSequence<LazyMapSequence<PredecessorList, Operand>, Value> {
    incomingPhiOperands.lazy.map { $0.value }
  }
}

public enum ArgumentConvention {
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
}

// Bridging utilities

extension BridgedArgument {
  public var argument: Argument { obj.getAs(Argument.self) }
  public var blockArgument: BlockArgument { obj.getAs(BlockArgument.self) }
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
