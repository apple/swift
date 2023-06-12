//===--- GlobalVariable.swift - Defines the GlobalVariable class ----------===//
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

final public class GlobalVariable : CustomStringConvertible, HasShortDescription, Hashable {
  public var name: StringRef {
    return StringRef(bridged: bridged.getName())
  }

  public var description: String {
    let stdString = bridged.getDebugDescription()
    return String(_cxxString: stdString)
  }

  public var shortDescription: String { name.string }

  public var isLet: Bool { bridged.isLet() }

  /// True, if the linkage of the global variable indicates that it is visible outside the current
  /// compilation unit and therefore not all of its uses are known.
  ///
  /// For example, `public` linkage.
  public var isPossiblyUsedExternally: Bool {
    return bridged.isPossiblyUsedExternally()
  }

  /// True, if the linkage of the global indicates that it has a definition outside the
  /// current compilation unit.
  ///
  /// For example, `public_external` linkage.
  public var isAvailableExternally: Bool {
    return bridged.isAvailableExternally()
  }

  public var staticInitValue: SingleValueInstruction? {
    bridged.getStaticInitializerValue().instruction as? SingleValueInstruction
  }

  /// True if the global's linkage and resilience expansion allow the global
  /// to be initialized statically.
  public var canBeInitializedStatically: Bool {
    return bridged.canBeInitializedStatically()
  }

  public static func ==(lhs: GlobalVariable, rhs: GlobalVariable) -> Bool {
    lhs === rhs
  }

  public func hash(into hasher: inout Hasher) {
    hasher.combine(ObjectIdentifier(self))
  }

  public var bridged: BridgedGlobalVar { BridgedGlobalVar(SwiftObject(self)) }
}

extension Instruction {
  public var isValidInStaticInitializerOfGlobal: Bool {
    // Rule out SILUndef and SILArgument.
    if operands.contains(where: { $0.value.definingInstruction == nil }) {
      return false
    }
    switch self {
    case let bi as BuiltinInst:
      switch bi.id {
      case .ZeroInitializer:
        let type = bi.type.isBuiltinVector ? bi.type.builtinVectorElementType : bi.type
        return type.isBuiltinInteger || type.isBuiltinFloat
      case .PtrToInt:
        return bi.operands[0].value is StringLiteralInst
      case .StringObjectOr:
        // The first operand can be a string literal (i.e. a pointer), but the
        // second operand must be a constant. This enables creating a
        // a pointer+offset relocation.
        // Note that StringObjectOr requires the or'd bits in the first
        // operand to be 0, so the operation is equivalent to an addition.
        return bi.operands[1].value is IntegerLiteralInst
      case .ZExtOrBitCast:
        return true;
      case .USubOver:
        // Handle StringObjectOr(tuple_extract(usub_with_overflow(x, offset)), bits)
        // This pattern appears in UTF8 String literal construction.
        if let tei = bi.uses.getSingleUser(ofType: TupleExtractInst.self),
           tei.isResultOfOffsetSubtract {
          return true
        }
        return false
      case .OnFastPath:
        return true
      default:
        return false
      }
    case let tei as TupleExtractInst:
      // Handle StringObjectOr(tuple_extract(usub_with_overflow(x, offset)), bits)
      // This pattern appears in UTF8 String literal construction.
      if tei.isResultOfOffsetSubtract,
         let bi = tei.uses.getSingleUser(ofType: BuiltinInst.self),
         bi.id == .StringObjectOr {
        return true
      }
      return false
    case let sli as StringLiteralInst:
      switch sli.encoding {
      case .Bytes, .UTF8:
        return true
      case .ObjCSelector:
        // Objective-C selector string literals cannot be used in static
        // initializers.
        return false
      }
    case let fri as FunctionRefInst:
      // TODO: support async function pointers in static globals.
      return !fri.referencedFunction.isAsync
    case is StructInst,
         is TupleInst,
         is EnumInst,
         is IntegerLiteralInst,
         is FloatLiteralInst,
         is ObjectInst,
         is ValueToBridgeObjectInst,
         is ConvertFunctionInst,
         is ThinToThickFunctionInst:
      return true
    default:
      return false
    }
  }
}

// Match the pattern:
// tuple_extract(usub_with_overflow(x, integer_literal, integer_literal 0), 0)
private extension TupleExtractInst {
  var isResultOfOffsetSubtract: Bool {
    if fieldIndex == 0,
       let bi = tuple as? BuiltinInst,
       bi.id == .USubOver,
       bi.operands[1].value is IntegerLiteralInst,
       let overFlowFlag = bi.operands[2].value as? IntegerLiteralInst,
       overFlowFlag.value.isNullValue() {
      return true
    }
    return false
  }
}

// Bridging utilities

extension BridgedGlobalVar {
  public var globalVar: GlobalVariable { obj.getAs(GlobalVariable.self) }

  public var optional: OptionalBridgedGlobalVar {
    OptionalBridgedGlobalVar(obj: self.obj)
  }
}

extension OptionalBridgedGlobalVar {
  public var globalVar: GlobalVariable? { obj.getAs(GlobalVariable.self) }

  public static var none: OptionalBridgedGlobalVar {
    OptionalBridgedGlobalVar(obj: nil)
  }
}
