//===--- Value.swift - the Value protocol ---------------------------------===//
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

@_semantics("arc.immortal")
public protocol Value : AnyObject, CustomStringConvertible {
  var uses: UseList { get }
  var type: Type { get }
  var ownership: Ownership { get }
  
  /// The instruction which defines the value.
  ///
  /// This is nil if the value is not defined by an instruction, e.g. an `Argument`.
  var definingInstruction: Instruction? { get }
  
  /// The block where the value is defined.
  ///
  /// It's not legal to get the definingBlock of an `Undef` value.
  var parentBlock: BasicBlock { get }

  /// True if the value has a trivial type.
  var hasTrivialType: Bool { get }

  /// True if the value has a trivial type which is and does not contain a Builtin.RawPointer.
  var hasTrivialNonPointerType: Bool { get }
}

public enum Ownership {
  /// A Value with `unowned` ownership kind is an independent value that
  /// has a lifetime that is only guaranteed to last until the next program
  /// visible side-effect. To maintain the lifetime of an unowned value, it
  /// must be converted to an owned representation via a copy_value.
  ///
  /// Unowned ownership kind occurs mainly along method/function boundaries in
  /// between Swift and Objective-C code.
  case unowned
  
  /// A Value with `owned` ownership kind is an independent value that has
  /// an ownership independent of any other ownership imbued within it. The
  /// Value must be paired with a consuming operation that ends the SSA
  /// value's lifetime exactly once along all paths through the program.
  case owned
  
  /// A Value with `guaranteed` ownership kind is an independent value that
  /// is guaranteed to be live over a specific region of the program. This
  /// region can come in several forms:
  ///
  /// 1. @guaranteed function argument. This guarantees that a value will
  /// outlive a function.
  ///
  /// 2. A shared borrow region. This is a region denoted by a
  /// begin_borrow/load_borrow instruction and an end_borrow instruction. The
  /// SSA value must not be destroyed or taken inside the borrowed region.
  ///
  /// Any value with guaranteed ownership must be paired with an end_borrow
  /// instruction exactly once along any path through the program.
  case guaranteed
  
  /// A Value with `none` ownership kind is an independent value outside of
  /// the ownership system. It is used to model values that are statically
  /// determined to be trivial. This includes trivially typed values as well
  /// as trivial cases of non-trivial enums. Naturally `none` can be merged with
  /// any ownership, allowing us to naturally model merge and branch
  /// points in the SSA graph, where more information about the value is
  /// statically available on some control flow paths.
  case none
  
  public var _bridged: BridgedValue.Ownership {
    switch self {
      case .unowned:    return BridgedValue.Ownership.Unowned
      case .owned:      return BridgedValue.Ownership.Owned
      case .guaranteed: return BridgedValue.Ownership.Guaranteed
      case .none:       return BridgedValue.Ownership.None
    }
  }
}

extension Value {
  public var description: String {
    let stdString = bridged.getDebugDescription()
    return String(_cxxString: stdString)
  }

  public var uses: UseList { UseList(bridged.getFirstUse()) }
  
  public var parentFunction: Function { parentBlock.parentFunction }

  public var type: Type { bridged.getType().type }

  /// True if the value has a trivial type.
  public var hasTrivialType: Bool { type.isTrivial(in: parentFunction) }

  /// True if the value has a trivial type which is and does not contain a Builtin.RawPointer.
  public var hasTrivialNonPointerType: Bool { type.isTrivialNonPointer(in: parentFunction) }

  public var ownership: Ownership {
    switch bridged.getOwnership() {
    case .Unowned:    return .unowned
    case .Owned:      return .owned
    case .Guaranteed: return .guaranteed
    case .None:       return .none
    default:
      fatalError("unsupported ownership")
    }
  }

  public var hashable: HashableValue { ObjectIdentifier(self) }

  public var bridged: BridgedValue {
    BridgedValue(obj: SwiftObject(self as AnyObject))
  }
}

public typealias HashableValue = ObjectIdentifier

// We can't make `Value` inherit from `Equatable`, since `Equatable` is a PAT,
// and we do use `Value` existentials around the codebase. Thus functions from
// `Equatable` are declared separately.
public func ==(_ lhs: Value, _ rhs: Value) -> Bool {
  return lhs === rhs
}

public func !=(_ lhs: Value, _ rhs: Value) -> Bool {
  return !(lhs === rhs)
}

/// A projected value, which is defined by the original value and a projection path.
///
/// For example, if the `value` is of type `struct S { var x: Int }` and `path` is `s0`,
/// then the projected value represents field `x` of the original value.
/// An empty path means represents the "whole" original value.
///
public struct ProjectedValue {
  public let value: Value
  public let path: SmallProjectionPath
}

extension Value {
  /// Returns a projected value, defined by this value and `path`.
  public func at(_ path: SmallProjectionPath) -> ProjectedValue {
    ProjectedValue(value: self, path: path)
  }

  /// Returns a projected value, defined by this value and path containig a single field of `kind` and `index`.
  public func at(_ kind: SmallProjectionPath.FieldKind, index: Int = 0) -> ProjectedValue {
    ProjectedValue(value: self, path: SmallProjectionPath(kind, index: index))
  }
}


extension BridgedValue {
  public var value: Value {
    // Doing the type check in C++ is much faster than a conformance lookup with `as! Value`.
    // And it makes a difference because this is a time critical function.
    switch getKind() {
      case .SingleValueInstruction:
        return obj.getAs(SingleValueInstruction.self)
      case .Argument:
        return obj.getAs(Argument.self)
      case .MultipleValueInstructionResult:
        return obj.getAs(MultipleValueInstructionResult.self)
      case .Undef:
        return obj.getAs(Undef.self)
      default:
        fatalError("unknown Value type")
    }
  }
}

public final class Undef : Value {
  public var definingInstruction: Instruction? { nil }

  public var parentBlock: BasicBlock {
    fatalError("undef has no defining block")
  }

  /// Undef has not parent function, therefore the default `hasTrivialType` does not work.
  /// Return the conservative default in this case.
  public var hasTrivialType: Bool { false }

  /// Undef has not parent function, therefore the default `hasTrivialNonPointerType` does not work.
  /// Return the conservative default in this case.
  public var hasTrivialNonPointerType: Bool { false }
}

final class PlaceholderValue : Value {
  public var definingInstruction: Instruction? { nil }
  public var parentBlock: BasicBlock {
    fatalError("PlaceholderValue has no defining block")
  }
}

extension OptionalBridgedValue {
  public var value: Value? { obj.getAs(AnyObject.self) as? Value }
}

extension Optional where Wrapped == Value {
  public var bridged: OptionalBridgedValue {
    OptionalBridgedValue(obj: self?.bridged.obj)
  }
}
