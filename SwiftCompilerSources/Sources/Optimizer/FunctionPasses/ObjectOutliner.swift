//===--- ObjectOutliner.swift ----------------------------------------------==//
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

/// Outlines COW objects from functions into statically initialized global variables.
/// This is currently only done for Arrays.
/// If a function constructs an Array literal with constant elements (done by storing
/// the element values into the array buffer), a new global variable is created which
/// contains the constant elements in its static initializer.
/// For example:
/// ```
///   public func arrayLookup(_ i: Int) -> Int {
///     let lookupTable = [10, 11, 12]
///     return lookupTable[i]
///   }
/// ```
/// is turned into
/// ```
///   private let outlinedVariable_from_arrayLookup = [10, 11, 12]  // statically initialized
///
///   public func arrayLookup(_ i: Int) -> Int {
///     return outlinedVariable_from_arrayLookup[i]
///   }
/// ```
///
/// As a second optimization, if the array is a string literal which is a parameter to the
/// `_findStringSwitchCase` library function and the array has many elements (> 16), the
/// call is redirected to `_findStringSwitchCaseWithCache`. This function builds a cache
/// (e.g. a Dictionary) and stores it into a global variable.
/// Then subsequent calls to this function can do a fast lookup using the cache.
///
let objectOutliner = FunctionPass(name: "object-outliner") {
  (function: Function, context: FunctionPassContext) in

  for inst in function.instructions {
    if let ari = inst as? AllocRefInstBase {
      if let globalValue = optimizeObjectAllocation(allocRef: ari, context) {
        optimizeFindStringCall(stringArray: globalValue, context)
      }
    }
  }
}

private func optimizeObjectAllocation(allocRef: AllocRefInstBase, _ context: FunctionPassContext) -> GlobalValueInst? {
  if !allocRef.fieldsKnownStatically {
    return nil
  }

  // The presence of an end_cow_mutation guarantees that the originally initialized
  // object is not mutated (because it must be copied before mutation).
  guard let endCOW = findEndCOWMutation(of: allocRef),
        !endCOW.doKeepUnique else {
    return nil
  }

  guard let (storesToClassFields, storesToTailElements) = getInitialization(of: allocRef, ignore: endCOW) else {
    return nil
  }

  let outlinedGlobal = context.createGlobalVariable(
        name: context.mangleOutlinedVariable(from: allocRef.parentFunction),
        type: allocRef.type, isPrivate: true)

  constructObject(of: allocRef, inInitializerOf: outlinedGlobal, storesToClassFields, storesToTailElements, context)
  context.erase(instructions: storesToClassFields)
  context.erase(instructions: storesToTailElements)

  return replace(object: allocRef, with: outlinedGlobal, endCOW, context)
}

private func findEndCOWMutation(of object: Value) -> EndCOWMutationInst? {
  for use in object.uses {
    switch use.instruction {
    case let uci as UpcastInst:
      if let ecm = findEndCOWMutation(of: uci) {
        return ecm
      }
    case let mv as MoveValueInst:
      if let ecm = findEndCOWMutation(of: mv) {
        return ecm
      }
    case let ecm as EndCOWMutationInst:
      return ecm
    default:
      break
    }
  }
  return nil
}

private func getInitialization(of allocRef: AllocRefInstBase,
                               ignore endCOW: EndCOWMutationInst) -> (storesToClassFields: [StoreInst],
                                                                      storesToTailElements: [StoreInst])? {
  guard let numTailElements = allocRef.numTailElements else {
    return nil
  }
  var fieldStores = Array<StoreInst?>(repeating: nil, count: allocRef.numClassFields)

  // If the tail element is a tuple, then its tuple elements are initialized with separate stores.
  // E.g:
  //   %2 = ref_tail_addr
  //   %3 = tuple_element_addr %2, 0
  //   store %0 to %3
  //   %4 = tuple_element_addr %2, 1
  //   store %1 to %4
  var tailStores = Array<StoreInst?>(repeating: nil, count: numTailElements * allocRef.numStoresPerTailElement)

  if !findInitStores(of: allocRef, &fieldStores, &tailStores, ignore: endCOW) {
    return nil
  }

  // Check that all fields and tail elements are initialized.
  if fieldStores.contains(nil) || tailStores.contains(nil) {
    return nil
  }
  return (fieldStores.map { $0! }, tailStores.map { $0! })
}

private func findInitStores(of object: Value,
                            _ fieldStores: inout [StoreInst?],
                            _ tailStores: inout [StoreInst?],
                            ignore endCOW: EndCOWMutationInst) -> Bool {
  for use in object.uses {
    switch use.instruction {
    case let uci as UpcastInst:
      if !findInitStores(of: uci, &fieldStores, &tailStores, ignore: endCOW) {
        return false
      }
    case let mvi as MoveValueInst:
      if !findInitStores(of: mvi, &fieldStores, &tailStores, ignore: endCOW) {
        return false
      }
    case let rea as RefElementAddrInst:
      if !findStores(inUsesOf: rea, index: rea.fieldIndex, stores: &fieldStores) {
        return false
      }
    case let rta as RefTailAddrInst:
      if !findStores(toTailAddress: rta, tailElementIndex: 0, stores: &tailStores) {
        return false
      }
    default:
      if !isValidUseOfObject(use.instruction, ignore: endCOW) {
        return false
      }
    }
  }
  return true
}

private func findStores(toTailAddress tailAddr: Value, tailElementIndex: Int, stores: inout [StoreInst?]) -> Bool {
  for use in tailAddr.uses {
    switch use.instruction {
    case let indexAddr as IndexAddrInst:
      guard let indexLiteral = indexAddr.index as? IntegerLiteralInst else {
        return false
      }
      let tailIdx = Int(indexLiteral.value.getZExtValue())
      if !findStores(toTailAddress: indexAddr, tailElementIndex: tailElementIndex + tailIdx, stores: &stores) {
        return false
      }
    case let tea as TupleElementAddrInst:
      // The tail elements are tuples. There is a separate store for each tuple element.
      let numTupleElements = tea.tuple.type.tupleElements.count
      let tupleIdx = tea.fieldIndex
      if !findStores(inUsesOf: tea, index: tailElementIndex * numTupleElements + tupleIdx, stores: &stores) {
        return false
      }
    case let store as StoreInst:
      if store.source.type.isTuple {
        // This kind of SIL is never generated because tuples are stored with separated stores to tuple_element_addr.
        // Just to be on the safe side..
        return false
      }
      if !handleStore(store, index: tailElementIndex, stores: &stores) {
        return false
      }
    default:
      if !isValidUseOfObject(use.instruction) {
        return false
      }
    }
  }
  return true
}

private func findStores(inUsesOf address: Value, index: Int, stores: inout [StoreInst?]) -> Bool {
  for use in address.uses {
    if let store = use.instruction as? StoreInst {
      if !handleStore(store, index: index, stores: &stores) {
        return false
      }
    } else if !isValidUseOfObject(use.instruction) {
      return false
    }
  }
  return true
}

private func handleStore(_ store: StoreInst, index: Int, stores: inout [StoreInst?]) -> Bool {
  if index >= 0 && index < stores.count,
     store.source.isValidGlobalInitValue,
     stores[index] == nil {
    stores[index] = store
    return true
  }
  return false
}

private func isValidUseOfObject(_ inst: Instruction, ignore endCOW: EndCOWMutationInst? = nil) -> Bool {
  if inst == endCOW {
    return true
  }

  switch inst {
  case is DebugValueInst,
       is LoadInst,
       is DeallocRefInst,
       is DeallocStackRefInst,
       is StrongRetainInst,
       is StrongReleaseInst,
       is FixLifetimeInst,
       is SetDeallocatingInst:
    return true

  case is StructElementAddrInst,
       is AddressToPointerInst,
       is StructInst,
       is TupleInst,
       is TupleExtractInst,
       is EnumInst,
       is StructExtractInst,
       is UncheckedRefCastInst,
       is UpcastInst:
    for use in (inst as! SingleValueInstruction).uses {
      if !isValidUseOfObject(use.instruction, ignore: endCOW) {
        return false
      }
    }
    return true

  case let bi as BuiltinInst:
    switch bi.id {
    case .ICMP_EQ, .ICMP_NE:
      // Handle the case for comparing addresses. This occurs when the Array
      // comparison function is inlined.
      return true
    case .DestroyArray:
      // We must not try to delete the tail allocated values. Although this would be a no-op
      // (because we only handle trivial types), it would be semantically wrong to apply this
      // builtin on the outlined object.
      return true
    default:
      return false
    }

  default:
    return false
  }
}

private func constructObject(of allocRef: AllocRefInstBase,
                             inInitializerOf global: GlobalVariable,
                             _ storesToClassFields: [StoreInst], _ storesToTailElements: [StoreInst],
                             _ context: FunctionPassContext) {
  var cloner = StaticInitCloner(cloneTo: global, context)
  defer { cloner.deinitialize() }

  // Create the initializers for the fields
  var objectArgs = [Value]()
  for store in storesToClassFields {
    objectArgs.append(cloner.clone(store.source as! SingleValueInstruction))
  }
  let globalBuilder = Builder(staticInitializerOf: global, context)

  // Create the initializers for the tail elements.
  let numTailTupleElems = allocRef.numStoresPerTailElement
  if numTailTupleElems > 1 {
    // The elements are tuples: combine numTailTupleElems elements to a single tuple instruction.
    for elementIdx in 0..<allocRef.numTailElements! {
      var tupleElems = [Value]()
      for tupleIdx in 0..<numTailTupleElems {
        let store = storesToTailElements[elementIdx * numTailTupleElems + tupleIdx]
        tupleElems.append(cloner.clone(store.source as! SingleValueInstruction))
      }
      let tuple = globalBuilder.createTuple(type: allocRef.tailAllocatedTypes[0], elements: tupleElems)
      objectArgs.append(tuple)
    }
  } else {
    // The non-tuple element case.
    for store in storesToTailElements {
      objectArgs.append(cloner.clone(store.source as! SingleValueInstruction))
    }
  }
  globalBuilder.createObject(type: allocRef.type, arguments: objectArgs, numBaseElements: storesToClassFields.count)

  // The initial value can contain a `begin_access` if it references another global variable by address, e.g.
  //   var p = Point(x: 10, y: 20)
  //   let a = [UnsafePointer(&p)]
  //
  global.stripAccessInstructionFromInitializer(context)
}

private func replace(object allocRef: AllocRefInstBase,
                     with global: GlobalVariable,
                     _ endCOW: EndCOWMutationInst, _ context: FunctionPassContext) -> GlobalValueInst {

  // Replace the alloc_ref by global_value + strong_retain instructions.
  let builder = Builder(before: allocRef, context)
  let globalValue = builder.createGlobalValue(global: global, isBare: false)
  builder.createStrongRetain(operand: globalValue)

  endCOW.uses.replaceAll(with: endCOW.instance, context)
  context.erase(instruction: endCOW)

  for use in allocRef.uses {
    let user = use.instruction
    switch user {
    case is SetDeallocatingInst:
      let builder = Builder(before: user, context)
      builder.createStrongRelease(operand: globalValue)
      context.erase(instruction: user)
    case is DeallocRefInst, is DeallocStackRefInst:
      context.erase(instruction: user)
    default:
      use.set(to: globalValue, context)
    }
  }
  context.erase(instruction: allocRef)
  return globalValue
}

private extension Value {
  /// Returns true if this value is a valid in a static initializer, including all its operands.
  var isValidGlobalInitValue: Bool {
    guard let svi = self as? SingleValueInstruction else {
      return false
    }
    if let beginAccess = svi as? BeginAccessInst {
      return beginAccess.address.isValidGlobalInitValue
    }
    if !svi.isValidInStaticInitializerOfGlobal {
      return false
    }
    for op in svi.operands {
      if !op.value.isValidGlobalInitValue {
        return false
      }
    }
    return true
  }
}

private extension AllocRefInstBase {
  var fieldsKnownStatically: Bool {
    if let allocDynamic = self as? AllocRefDynamicInst,
       !allocDynamic.isDynamicTypeDeinitAndSizeKnownEquivalentToBaseType {
      return false
    }
    if isObjC {
      return false
    }
    return true
  }

  var numTailElements: Int? {
    // We only support a single tail allocated array.
    // Stdlib's tail allocated arrays don't have any side-effects in the constructor if the element type is trivial.
    // TODO: also exclude custom tail allocated arrays which might have side-effects in the destructor.
    if tailAllocatedCounts.count != 1 {
      return nil
    }

    // The number of tail allocated elements must be constant.
    guard let tailCountLiteral = tailAllocatedCounts[0].value as? IntegerLiteralInst,
          tailCountLiteral.value.getActiveBits() <= 20 else {
      return nil
    }
    return Int(tailCountLiteral.value.getZExtValue());
  }

  var numClassFields: Int {
    assert(type.isClass)
    return type.getNominalFields(in: parentFunction).count
  }

  var numStoresPerTailElement: Int {
    let tailType = tailAllocatedTypes[0]
    if tailType.isTuple {
      return tailType.tupleElements.count
    }
    return 1
  }
}

private extension FunctionPassContext {
  func erase(instructions: [Instruction]) {
    for inst in instructions {
      erase(instruction: inst)
    }
  }
}

private func optimizeFindStringCall(stringArray: GlobalValueInst, _ context: FunctionPassContext) {
  if stringArray.numArrayElements > 16,
     let findStringCall = findFindStringCall(stringArray: stringArray),
     let cachedFindStringFunc = getFindStringSwitchCaseWithCacheFunction(context) {
    replace(findStringCall: findStringCall, with: cachedFindStringFunc, context)
  }
}

/// Finds a call to findStringSwitchCase which takes `stringArray` as parameter.
private func findFindStringCall(stringArray: Value) -> ApplyInst? {
  for use in stringArray.uses {
    switch use.instruction {
    case let apply as ApplyInst:
      // There should only be a single call to findStringSwitchCase. But even
      // if there are multiple calls, it's not problem - we'll just optimize the
      // last one we find.
      if apply.hasSemanticsAttribute("findStringSwitchCase") {
        return apply
      }
    case is StructInst,
         is TupleInst,
         is UncheckedRefCastInst,
         is UpcastInst:
      if let foundCall = findFindStringCall(stringArray: use.instruction as! SingleValueInstruction) {
        return foundCall
      }
    default:
      break
    }
  }
  return nil
}

private func getFindStringSwitchCaseWithCacheFunction(_ context: FunctionPassContext) -> Function? {
  if let f = context.lookupStdlibFunction(name: "_findStringSwitchCaseWithCache"),
     f.argumentTypes.count == 3 {
    return f
  }
  return nil
}

private func replace(findStringCall: ApplyInst,
                     with cachedFindStringFunc: Function,
                     _ context: FunctionPassContext) {
  let cacheType = cachedFindStringFunc.argumentTypes[2].objectType
  let wordTy = cacheType.getNominalFields(in: findStringCall.parentFunction)[0]

  let name = context.mangleOutlinedVariable(from: findStringCall.parentFunction)

  // Create an "opaque" global variable which is passed as inout to
  // _findStringSwitchCaseWithCache and into which the function stores the "cache".
  let cacheVar = context.createGlobalVariable(name: name, type: cacheType, isPrivate: true)

  let varBuilder = Builder(staticInitializerOf: cacheVar, context)
  let zero = varBuilder.createIntegerLiteral(0, type: wordTy)
  _ = varBuilder.createStruct(type: cacheType, elements: [zero, zero])

  let builder = Builder(before: findStringCall, context)
  let cacheAddr = builder.createGlobalAddr(global: cacheVar)
  let findStringRef = builder.createFunctionRef(cachedFindStringFunc)
  let newCall = builder.createApply(function: findStringRef, SubstitutionMap(),
                                    arguments: [findStringCall.arguments[0],
                                                findStringCall.arguments[1],
                                                cacheAddr])

  findStringCall.uses.replaceAll(with: newCall, context)
  context.erase(instruction: findStringCall)
}

private extension GlobalValueInst {
  /// Assuming the global is an Array, returns the number of elements = tail elements.
  var numArrayElements: Int {
    (global.staticInitValue! as! ObjectInst).tailOperands.count
  }
}
