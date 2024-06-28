//===--- SimplifyLoad.swift -----------------------------------------------===//
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

extension LoadInst : OnoneSimplifyable, SILCombineSimplifyable {
  func simplify(_ context: SimplifyContext) {
    if optimizeLoadOfAddrUpcast(context) {
      return
    }
    if optimizeLoadFromStringLiteral(context) {
      return
    }
    if optmizeLoadFromEmptyCollection(context) {
      return
    }
    if replaceLoadOfGlobalLet(context) {
      return
    }
    removeIfDead(context)
  }

  /// ```
  ///   %1 = unchecked_addr_cast %0 : $*DerivedClass to $*BaseClass
  ///   %2 = load %1
  /// ```
  /// is transformed to
  /// ```
  ///   %1 = load %0 : $*BaseClass
  ///   %2 = upcast %1 : $DerivedClass to $BaseClass
  /// ```
  private func optimizeLoadOfAddrUpcast(_ context: SimplifyContext) -> Bool {
    if let uac = address as? UncheckedAddrCastInst,
       uac.type.isExactSuperclass(of: uac.fromAddress.type),
       uac.type != uac.fromAddress.type {

      operand.set(to: uac.fromAddress, context)
      let builder = Builder(before: self, context)
      let newLoad = builder.createLoad(fromAddress: uac.fromAddress, ownership: loadOwnership)
      let cast = builder.createUpcast(from: newLoad, to: type)
      uses.replaceAll(with: cast, context)
      context.erase(instruction: self)
      return true
    }
    return false
  }

  /// The load from a string literal element, e.g.
  /// ```
  ///   %0 = string_literal utf8 "abc"
  ///   %1 = integer_literal $Builtin.Word, 1
  ///   %2 = pointer_to_address %0 : $Builtin.RawPointer to [strict] $*UInt8
  ///   %3 = index_addr %2 : $*UInt8, %1 : $Builtin.Word
  ///   %4 = struct_element_addr %3 : $*UInt8, #UInt8._value
  ///   %5 = load %4 : $*Builtin.Int8
  /// ```
  /// is replaced by the character value, e.g. `98` in this example.
  ///
  private func optimizeLoadFromStringLiteral(_ context: SimplifyContext) -> Bool {
    if self.type.isBuiltinInteger(withFixedWidth: 8),
       let sea = self.address as? StructElementAddrInst,
       let (baseAddr, index) = sea.struct.getBaseAddressAndOffset(),
       let pta = baseAddr as? PointerToAddressInst,
       let stringLiteral = pta.pointer as? StringLiteralInst,
       stringLiteral.encoding == .UTF8,
       index < stringLiteral.value.count {

      let builder = Builder(before: self, context)
      let charLiteral = builder.createIntegerLiteral(Int(stringLiteral.value[index]), type: type)
      uses.replaceAll(with: charLiteral, context)
      context.erase(instruction: self)
      return true
    }
    return false
  }

  /// Loading `count` or `capacity` from the empty `Array`, `Set` or `Dictionary` singleton
  /// is replaced by a 0 literal.
  private func optmizeLoadFromEmptyCollection(_ context: SimplifyContext) -> Bool {
    if self.isZeroLoadFromEmptyCollection() {
      let builder = Builder(before: self, context)
      let zeroLiteral = builder.createIntegerLiteral(0, type: type)
      uses.replaceAll(with: zeroLiteral, context)
      context.erase(instruction: self)
      return true
    }
    return false
  }

  /// The load of a global let variable is replaced by its static initializer value.
  private func replaceLoadOfGlobalLet(_ context: SimplifyContext) -> Bool {
    guard let globalInitVal = getGlobalInitValue(address: address, context) else {
      return false
    }
    if !globalInitVal.canBeCopied(into: parentFunction, context) {
      return false
    }
    var cloner = StaticInitCloner(cloneBefore: self, context)
    defer { cloner.deinitialize() }

    let initVal = cloner.clone(globalInitVal)

    uses.replaceAll(with: initVal, context)
    // Also erases a builtin "once" on which the global_addr depends on. This is fine
    // because we only replace the load if the global init function doesn't have any side effect.
    transitivelyErase(load: self, context)
    return true
  }

  private func isZeroLoadFromEmptyCollection() -> Bool {
    if !type.isBuiltinInteger {
      return false
    }
    var addr = address

    // Find the root object of the load-address.
    while true {
      switch addr {
      case let ga as GlobalAddrInst:
        switch ga.global.name {
        case "_swiftEmptyArrayStorage",
             "_swiftEmptyDictionarySingleton",
             "_swiftEmptySetSingleton":
          return true
        default:
          return false
        }
      case let sea as StructElementAddrInst:
        let structType = sea.struct.type
        if structType.nominal.name == "_SwiftArrayBodyStorage" {
          guard let fields = structType.getNominalFields(in: parentFunction) else {
            return false
          }
          switch fields.getNameOfField(withIndex: sea.fieldIndex) {
          case "count":
            break
          case "_capacityAndFlags":
            if uses.contains(where: { !$0.instruction.isShiftRightByAtLeastOne }) {
              return false
            }
          default:
            return false
          }
        }
        addr = sea.struct
      case let rea as RefElementAddrInst:
        let classType = rea.instance.type
        switch classType.nominal.name {
        case "__RawDictionaryStorage",
              "__RawSetStorage":
          // For Dictionary and Set we support "count" and "capacity".
          guard let fields = classType.getNominalFields(in: parentFunction) else {
            return false
          }
          switch fields.getNameOfField(withIndex: rea.fieldIndex) {
          case "_count", "_capacity":
            break
          default:
            return false
          }
        default:
          break
        }
        addr = rea.instance
      case is UncheckedRefCastInst,
           is UpcastInst,
           is RawPointerToRefInst,
           is AddressToPointerInst,
           is BeginBorrowInst,
           is CopyValueInst,
           is EndCOWMutationInst:
        addr = (addr as! UnaryInstruction).operand.value
      case let mviResult as MultipleValueInstructionResult:
        guard let bci = mviResult.parentInstruction as? BeginCOWMutationInst else {
          return false
        }
        addr = bci.instance
      default:
        return false
      }
    }
  }

  /// Removes the `load [copy]` if the loaded value is just destroyed.
  private func removeIfDead(_ context: SimplifyContext) {
    if loadOwnership == .copy,
       loadedValueIsDead(context) {
      for use in uses {
        context.erase(instruction: use.instruction)
      }
      context.erase(instruction: self)
    }
  }

  private func loadedValueIsDead(_ context: SimplifyContext) -> Bool {
    if context.preserveDebugInfo {
      return !uses.contains { !($0.instruction is DestroyValueInst) }
    } else {
      return !uses.ignoreDebugUses.contains { !($0.instruction is DestroyValueInst) }
    }
  }
}

/// Returns the init value of a global which is loaded from `address`.
private func getGlobalInitValue(address: Value, _ context: SimplifyContext) -> Value? {
  switch address {
  case let gai as GlobalAddrInst:
    if gai.global.isLet {
      if let staticInitValue = gai.global.staticInitValue {
        return staticInitValue
      }
      if let staticInitValue = getInitializerFromInitFunction(of: gai, context) {
        return staticInitValue
      }
    }
  case let pta as PointerToAddressInst:
    return globalLoadedViaAddressor(pointer: pta.pointer)?.staticInitValue
  case let sea as StructElementAddrInst:
    if let structVal = getGlobalInitValue(address: sea.struct, context) as? StructInst {
      return structVal.operands[sea.fieldIndex].value
    }
  case let tea as TupleElementAddrInst:
    if let tupleVal = getGlobalInitValue(address: tea.tuple, context) as? TupleInst {
      return tupleVal.operands[tea.fieldIndex].value
    }
  case let bai as BeginAccessInst:
    return getGlobalInitValue(address: bai.address, context)
  default:
    break
  }
  return nil
}

private func getInitializerFromInitFunction(of globalAddr: GlobalAddrInst, _ context: SimplifyContext) -> Value? {
  guard let dependentOn = globalAddr.dependencyToken,
        let builtinOnce = dependentOn as? BuiltinInst,
        builtinOnce.id == .Once,
        let initFnRef = builtinOnce.operands[1].value as? FunctionRefInst else
  {
    return nil
  }
  let initFn = initFnRef.referencedFunction
  context.notifyDependency(onBodyOf: initFn)
  guard let (_, storeToGlobal) = getGlobalInitialization(of: initFn, forStaticInitializer: false, context) else {
    return nil
  }
  return storeToGlobal.source
}

private func globalLoadedViaAddressor(pointer: Value) -> GlobalVariable? {
  if let ai = pointer as? ApplyInst,
     let callee = ai.referencedFunction,
     let global = callee.globalOfGlobalInitFunction,
     global.isLet {
    return global
  }
  return nil
}

private func transitivelyErase(load: LoadInst, _ context: SimplifyContext) {
  var inst: SingleValueInstruction = load
  while inst.uses.isEmpty {
    if inst.operands.count != 1 {
      context.erase(instruction: inst)
      return
    }
    let operandInst = inst.operands[0].value as! SingleValueInstruction
    context.erase(instruction: inst)
    inst = operandInst
  }
}

private extension Value {
  func canBeCopied(into function: Function, _ context: SimplifyContext) -> Bool {
    if !function.isAnySerialized {
      return true
    }

    // Can't use `ValueSet` because the this value is inside a global initializer and
    // not inside a function.
    var worklist = Stack<Value>(context)
    defer { worklist.deinitialize() }

    var handled = Set<ObjectIdentifier>()

    worklist.push(self)
    handled.insert(ObjectIdentifier(self))

    while let value = worklist.pop() {
      if let fri = value as? FunctionRefInst {
        if !fri.referencedFunction.hasValidLinkageForFragileRef(function.serializedKind) {
          return false
        }
      }
      for op in value.definingInstruction!.operands {
        if handled.insert(ObjectIdentifier(op.value)).inserted {
          worklist.push(op.value)
        }
      }
    }
    return true
  }

  func getBaseAddressAndOffset() -> (baseAddress: Value, offset: Int)? {
    if let indexAddr = self as? IndexAddrInst {
      guard let indexLiteral = indexAddr.index as? IntegerLiteralInst,
            let indexValue = indexLiteral.value else
      {
        return nil
      }
      return (baseAddress: indexAddr.base, offset: indexValue)
    }
    return (baseAddress: self, offset: 0)
  }
}

private extension Instruction {
  var isShiftRightByAtLeastOne: Bool {
    guard let bi = self as? BuiltinInst,
          bi.id == .LShr,
          let shiftLiteral = bi.operands[1].value as? IntegerLiteralInst,
          let shiftValue = shiftLiteral.value else
    {
      return false
    }
    return shiftValue > 0
  }
}
