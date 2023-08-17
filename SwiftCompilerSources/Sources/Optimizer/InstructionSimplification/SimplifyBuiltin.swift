//===--- SimplifyBuiltin.swift --------------------------------------------===//
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

import SIL

extension BuiltinInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    switch id {
      case .IsConcrete:
        // Don't constant fold a Builtin.isConcrete of a type with archetypes in the middle
        // of the pipeline, because a generic specializer might run afterwards which turns the
        // type into a concrete type.
        optimizeIsConcrete(allowArchetypes: false, context)
      case .IsSameMetatype:
        optimizeIsSameMetatype(context)
      case .Once:
        optimizeBuiltinOnce(context)
      case .CanBeObjCClass:
        optimizeCanBeClass(context)
      case .AssertConf:
        optimizeAssertConfig(context)
      case .Sizeof,
           .Strideof,
           .Alignof:
        optimizeTargetTypeConst(context)
      default:
        if let literal = constantFold(context) {
          uses.replaceAll(with: literal, context)
        }
    }
  }
}

extension BuiltinInst : LateOnoneSimplifyable {
  func simplifyLate(_ context: SimplifyContext) {
    if id == .IsConcrete {
      // At the end of the pipeline we can be sure that the isConcrete's type doesn't get "more" concrete.
      optimizeIsConcrete(allowArchetypes: true, context)
    } else {
      simplify(context)
    }
  }
}

private extension BuiltinInst {
  func optimizeIsConcrete(allowArchetypes: Bool, _ context: SimplifyContext) {
    let hasArchetype = operands[0].value.type.hasArchetype
    if hasArchetype && !allowArchetypes {
      return
    }
    let builder = Builder(before: self, context)
    let result = builder.createIntegerLiteral(hasArchetype ? 0 : 1, type: type)
    uses.replaceAll(with: result, context)
    context.erase(instruction: self)
  }

  func optimizeIsSameMetatype(_ context: SimplifyContext) {
    let lhs = operands[0].value
    let rhs = operands[1].value

    guard let equal = typesOfValuesAreEqual(lhs, rhs, in: parentFunction) else {
      return
    }
    let builder = Builder(before: self, context)
    let result = builder.createIntegerLiteral(equal ? 1 : 0, type: type)

    uses.replaceAll(with: result, context)
  }

  func optimizeBuiltinOnce(_ context: SimplifyContext) {
    guard let callee = calleeOfOnce, callee.isDefinition else {
      return
    }
    // If the callee is side effect-free we can remove the whole builtin "once".
    // We don't use the callee's memory effects but instead look at all callee instructions
    // because memory effects are not computed in the Onone pipeline, yet.
    // This is no problem because the callee (usually a global init function )is mostly very small,
    // or contains the side-effect instruction `alloc_global` right at the beginning.
    if callee.instructions.contains(where: hasSideEffectForBuiltinOnce) {
      return
    }
    context.erase(instruction: self)
  }

  var calleeOfOnce: Function? {
    let callee = operands[1].value
    if let fri = callee as? FunctionRefInst {
      return fri.referencedFunction
    }
    return nil
  }

  func optimizeCanBeClass(_ context: SimplifyContext) {
    guard let ty = substitutionMap.replacementTypes[0] else {
      return
    }
    let literal: IntegerLiteralInst
    switch ty.canBeClass {
    case .IsNot:
      let builder = Builder(before: self, context)
      literal = builder.createIntegerLiteral(0,  type: type)
    case .Is:
      let builder = Builder(before: self, context)
      literal = builder.createIntegerLiteral(1,  type: type)
    case .CanBe:
      return
    default:
      fatalError()
    }
    uses.replaceAll(with: literal, context)
    context.erase(instruction: self)
  }

  func optimizeAssertConfig(_ context: SimplifyContext) {
    let literal: IntegerLiteralInst
    switch context.options.assertConfiguration {
    case .enabled:
      let builder = Builder(before: self, context)
      literal = builder.createIntegerLiteral(1,  type: type)
    case .disabled:
      let builder = Builder(before: self, context)
      literal = builder.createIntegerLiteral(0,  type: type)
    default:
      return
    }
    uses.replaceAll(with: literal, context)
    context.erase(instruction: self)
  }
  
  func optimizeTargetTypeConst(_ context: SimplifyContext) {
    guard let ty = substitutionMap.replacementTypes[0] else {
      return
    }
    
    let value: Int?
    switch id {
    case .Sizeof:
      value = ty.getStaticSize(context: context)
    case .Strideof:
      value = ty.getStaticStride(context: context)
    case .Alignof:
      value = ty.getStaticAlignment(context: context)
    default:
      fatalError()
    }
    
    guard let value else {
      return
    }
    
    let builder = Builder(before: self, context)
    let literal = builder.createIntegerLiteral(value, type: type)
    uses.replaceAll(with: literal, context)
    context.erase(instruction: self)
  }
}

private func hasSideEffectForBuiltinOnce(_ instruction: Instruction) -> Bool {
  switch instruction {
  case is DebugStepInst, is DebugValueInst:
    return false
  default:
    return instruction.mayReadOrWriteMemory ||
           instruction.hasUnspecifiedSideEffects
  }
}

private func typesOfValuesAreEqual(_ lhs: Value, _ rhs: Value, in function: Function) -> Bool? {
  if lhs == rhs {
    return true
  }

  guard let lhsExistential = lhs as? InitExistentialMetatypeInst,
        let rhsExistential = rhs as? InitExistentialMetatypeInst else {
    return nil
  }

  let lhsTy = lhsExistential.metatype.type.instanceTypeOfMetatype(in: function)
  let rhsTy = rhsExistential.metatype.type.instanceTypeOfMetatype(in: function)

  // Do we know the exact types? This is not the case e.g. if a type is passed as metatype
  // to the function.
  let typesAreExact = lhsExistential.metatype is MetatypeInst &&
                      rhsExistential.metatype is MetatypeInst

  switch (lhsTy.typeKind, rhsTy.typeKind) {
  case (_, .unknown), (.unknown, _):
    return nil
  case (let leftKind, let rightKind) where leftKind != rightKind:
    // E.g. a function type is always different than a struct, regardless of what archetypes
    // the two types may contain.
    return false
  case (.struct, .struct), (.enum, .enum):
    // Two different structs/enums are always not equal, regardless of what archetypes
    // the two types may contain.
    if lhsTy.nominal != rhsTy.nominal {
      return false
    }
  case (.class, .class):
    // In case of classes this only holds if we know the exact types.
    // Otherwise one class could be a sub-class of the other class.
    if typesAreExact && lhsTy.nominal != rhsTy.nominal {
      return false
    }
  default:
    break
  }

  if !typesAreExact {
    // Types which e.g. come from type parameters may differ at runtime while the declared AST types are the same.
    return nil
  }

  if lhsTy.hasArchetype || rhsTy.hasArchetype {
    // We don't know anything about archetypes. They may be identical at runtime or not.
    // We could do something more sophisticated here, e.g. look at conformances. But for simplicity,
    // we are just conservative.
    return nil
  }

  // Generic ObjectiveC class, which are specialized for different NSObject types have different AST types
  // but the same runtime metatype.
  if lhsTy.isOrContainsObjectiveCClass || rhsTy.isOrContainsObjectiveCClass {
    return nil
  }

  return lhsTy == rhsTy
}

private extension Type {
  enum TypeKind {
    case `struct`, `class`, `enum`, tuple, function, unknown
  }

  var typeKind: TypeKind {
    if isStruct  { return .struct }
    if isClass  { return .class }
    if isEnum  { return .enum }
    if isTuple    { return .tuple }
    if isFunction { return .function }
    return .unknown
  }
}
