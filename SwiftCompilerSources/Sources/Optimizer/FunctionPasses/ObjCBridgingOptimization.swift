//===--- ObjCBridgingOptimization.swift - optimize ObjC bridging ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

/// Removes redundant ObjectiveC <-> Swift bridging calls.
///
/// Basically, if a value is bridged from ObjectiveC to Swift an then back to ObjectiveC
/// again, then just re-use the original ObjectiveC value.
///
/// Things get a little bit more complicated in case of optionals (Nullable pointers).
/// In this case both bridging calls are embedded in an `switch_enum` CFG diamond, like
/// ```
///   switch_enum %originalOptionalObjcValue
/// some_bb(%1):
///   %2 = enum #some(%1)
///   %3 = apply %bridgeFromObjc(%2)
///   %4 = enum #some(%3)
///   br continue_bb(%4)
/// none_bb:
///   %5 = enum #none
///   br continue_bb(%5)
/// continue_bb(%bridgedOptionalSwiftValue):
/// ```
let objCBridgingOptimization = FunctionPass(name: "objc-bridging-opt") {
  (function: Function, context: FunctionPassContext) in

  if !function.hasOwnership { return }

  // First try to optimize the optional -> optional case.
  // We need to do this before handling the non-optional case to prevent
  // sub-optimal optimization of bridging calls inside a switch_enum.
  for block in function.blocks {
    // Start at a block argument, which is the "result" of the switch_enum CFG diamond.
    if !optimizeOptionalBridging(forArgumentOf: block, context) {
      return
    }
  }

  // Now try to optimize non-optional and optional -> non-optional bridging.
  for inst in function.instructions {
    if let apply = inst as? ApplyInst {
      if !optimizeNonOptionalBridging(apply, context) {
        return
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                      Top-level optimization functions
//===----------------------------------------------------------------------===//

/// Optimizes redundant bridging calls where both calls are within `switch_enum` diamonds.
///
/// For example:
/// ```
///   let s = returnOptionalNSString()
///   useOptionalNSString(s)
/// ```
///
/// The `block` is the continue-block of the second `switch_enum` diamond.
/// Returns true if the pass should continue running.
private func optimizeOptionalBridging(forArgumentOf block: BasicBlock,
                                      _ context: FunctionPassContext) -> Bool {
  if block.arguments.count != 1 {
    // For simplicity only handle the common case: there is only one phi-argument which
    // is the result of the bridging operation.
    return true
  }
  // Check for the second swift -> ObjC bridging operation.
  let finalObjCValue = block.arguments[0]
  guard let swiftValueSwitch = isOptionalBridging(of: finalObjCValue, isBridging: isBridgeToObjcCall) else {
    return true
  }

  // Check for the first ObjC -> swift bridging operation.
  let swiftValue = lookThroughOwnershipInsts(swiftValueSwitch.enumOp)
  guard let originalObjCValueSwitch = isOptionalBridging(of: swiftValue, isBridging: isBridgeToSwiftCall) else {
    return true
  }
  
  let originalObjCValue = originalObjCValueSwitch.enumOp
  if finalObjCValue.type != originalObjCValue.type {
    return true
  }

  if !context.continueWithNextSubpassRun(for: originalObjCValueSwitch) {
    return false
  }

  // The second bridging operation can be in a different control region than the first one,
  // e.g. it can be in a loop whereas the first is not in that loop. Therefore we have to
  // copy + makeAvailable.
  let replacement = originalObjCValue.copy(at: originalObjCValueSwitch,
                                           andMakeAvailableIn: block, context)

  finalObjCValue.uses.replaceAll(with: replacement, context)
  block.eraseArgument(at: 0, context)
  
  // The swift -> ObjC bridging call has no `readonly` attribute, therefore we have to
  // explicitly delete it. The ObjC -> swift call has such an attribute and will be removed
  // buy a later dead-code elimination pass.
  removeBridgingCodeInPredecessors(of: block, context)
  return true
}

/// Optimizes redundant bridging calls where the second call is a non-optional bridging operation,
/// i.e. is _not_ within `switch_enum` diamond.
///
/// The `apply` is the second (swift -> ObjC) bridging call.
/// Returns true if the pass should continue running.
private func optimizeNonOptionalBridging(_ apply: ApplyInst,
                                         _ context: FunctionPassContext) -> Bool {
                                              
  guard let bridgeToObjcCall = isBridgeToObjcCall(apply) else {
    return true
  }

  let swiftValue = lookThroughOwnershipInsts(bridgeToObjcCall.arguments[0])

  // Handle the first case: the ObjC -> swift bridging operation is optional and the swift -> ObjC
  // bridging is within a test for Optional.some, e.g.
  // ```
  //   if let s = returnOptionalNSString() {
  //     useNonOptionalNSString(s)
  //   }
  // ```
  if let (se, someCase) = isPayloadOfSwitchEnum(swiftValue),
     let originalObjCValueSwitch = isOptionalBridging(of: se.enumOp, isBridging: isBridgeToSwiftCall) {

    if !context.continueWithNextSubpassRun(for: originalObjCValueSwitch) {
      return false
    }

    let originalObjCValue = originalObjCValueSwitch.enumOp
    let optionalReplacement = originalObjCValue.copy(at: originalObjCValueSwitch,
                                                     andMakeAvailableIn: bridgeToObjcCall.parentBlock,
                                                     context)
    let builder = Builder(before: bridgeToObjcCall, context)
    
    // We know that it's the some-case.
    let replacement = builder.createUncheckedEnumData(enum: optionalReplacement,
                                                      caseIndex: someCase,
                                                      resultType: bridgeToObjcCall.type)
    bridgeToObjcCall.uses.replaceAll(with: replacement, context)
    context.erase(instruction: bridgeToObjcCall)
    return true
  }

  // Handle the second case: both bridging calls are non-optional, e.g.
  // ```
  //   let s = returnNonOptionalNSString()
  //   useNonOptionalNSString(s)
  // ```
  guard let bridgeToSwiftCall = isBridgeToSwiftCall(swiftValue) else {
    return true
  }

  if !context.continueWithNextSubpassRun(for: bridgeToSwiftCall) {
    return false
  }

  let originalObjCValue = bridgeToSwiftCall.arguments[0]
  let optionalObjCType = originalObjCValue.type
  
  // The bridging functions from ObjC -> Swift take an optional argument and return a
  // non-optional Swift value. In the nil-case they return an empty (e.g. empty String,
  // empty Array, etc.) swift value.
  // We have to replicate that behavior here.

  guard let someCase = optionalObjCType.getIndexOfEnumCase(withName: "some") else { return true }
  guard let noneCase = optionalObjCType.getIndexOfEnumCase(withName: "none") else { return true }

  // Creates a `switch_enum` on `originalObjCValue` and in the nil-case return a bridged
  // empty value.
  // Create the needed blocks of the `switch_enum` CFG diamond.
  let origBlock = bridgeToSwiftCall.parentBlock
  let someBlock = context.splitBlock(at: bridgeToSwiftCall)
  let noneBlock = context.splitBlock(at: bridgeToSwiftCall)
  let continueBlock = context.splitBlock(at: bridgeToSwiftCall)


  let builder = Builder(atEndOf: origBlock, location: bridgeToSwiftCall.location, context)
  let copiedValue = builder.createCopyValue(operand: originalObjCValue)
  builder.createSwitchEnum(enum: copiedValue, cases: [(someCase, someBlock),
                                                       (noneCase, noneBlock)])

  // The nil case: call the ObjC -> Swift bridging function, which will return
  // an empty swift value.
  let noneBuilder = Builder(atEndOf: noneBlock, location: bridgeToSwiftCall.location, context)
  let subst = bridgeToObjcCall.substitutionMap
  let emptySwiftValue = noneBuilder.createApply(
        function: bridgeToSwiftCall.callee,
        bridgeToSwiftCall.substitutionMap, arguments: Array(bridgeToSwiftCall.arguments))
  // ... and bridge that to ObjectiveC.
  let emptyObjCValue = noneBuilder.createApply(
        function: noneBuilder.createFunctionRef(bridgeToObjcCall.referencedFunction!),
        subst, arguments: [emptySwiftValue])
  noneBuilder.createDestroyValue(operand: emptySwiftValue)
  noneBuilder.createBranch(to: continueBlock, arguments: [emptyObjCValue])

  // In the some-case just forward the original NSString.
  let objCType = emptyObjCValue.type
  let forwardedValue = someBlock.addBlockArgument(type: objCType, ownership: .owned, context)
  let someBuilder = Builder(atEndOf: someBlock, location: bridgeToSwiftCall.location, context)
  someBuilder.createBranch(to: continueBlock, arguments: [forwardedValue])

  let s = continueBlock.addBlockArgument(type: objCType, ownership: .owned, context)
  
  // Now replace the bridged value with the original value in the destination block.
  let replacement = s.makeAvailable(in: bridgeToObjcCall.parentBlock, context)
  bridgeToObjcCall.uses.replaceAll(with: replacement, context)
  context.erase(instruction: bridgeToObjcCall)
  return true
}

//===----------------------------------------------------------------------===//
//                               Utility functions
//===----------------------------------------------------------------------===//

/// Removes `enum` instructions and bridging calls in all predecessors of `block`.
private func removeBridgingCodeInPredecessors(of block: BasicBlock, _ context: FunctionPassContext) {
  for pred in block.predecessors {
    let branch = pred.terminator as! BranchInst
    let builder = Builder(after: branch, context)
    builder.createBranch(to: block)
    
    let en = branch.operands[0].value as! EnumInst
    context.erase(instruction: branch)
    let payload = en.payload
    context.erase(instruction: en)
    if let bridgingCall = payload {
      context.erase(instruction: bridgingCall as! ApplyInst)
    }
  }
}

private func lookThroughOwnershipInsts(_ value: Value) -> Value {
  // Looks like it's sufficient to support begin_borrow and copy_value for now.
  // TODO: add move_value if needed.
  if let bbi = value as? BeginBorrowInst {
    return bbi.borrowedValue
  }
  if let cvi = value as? CopyValueInst {
    return cvi.fromValue
  }
  return value
}

/// Checks for an optional bridging `switch_enum` diamond.
///
/// ```
///   switch_enum %0             // returned instruction
/// some_bb(%1):
///   %2 = enum #some(%1)        // only in case of ObjC -> Swift briding
///   %3 = apply %bridging(%2)   // returned by `isBridging`
///   %4 = enum #some(%3)
///   br continue_bb(%4)
/// none_bb:
///   %5 = enum #none
///   br continue_bb(%5)
/// continue_bb(%value):         // passed value
/// ```
private func isOptionalBridging(of value: Value, isBridging: (Value) -> ApplyInst?) -> SwitchEnumInst? {
  guard let arg = value as? BlockArgument,
        arg.isPhiArgument else {
    return nil
  }
  
  var noneSwitch: SwitchEnumInst?
  var someSwitch: SwitchEnumInst?
  
  // Check if one incoming value is the none-case and the other is the some-case.
  for incomingVal in arg.incomingPhiValues {
    // In both branches, the result must be an `enum` which is passed to the
    // continue_bb's phi-argument.
    guard let enumInst = incomingVal as? EnumInst,
          let singleEnumUse = enumInst.uses.singleUse,
          singleEnumUse.instruction is BranchInst else {
      return nil
    }
    if let payload = enumInst.payload {
      // The some-case
      if someSwitch != nil { return nil }
      guard let bridgingCall = isBridging(payload),
            bridgingCall.uses.isSingleUse else {
        return nil
      }
      let callArgument = bridgingCall.arguments[0]
      
      // If it's an ObjC -> Swift bridging call the argument is wrapped into an optional enum.
      if callArgument.type.isEnum {
        guard let sourceEnum = callArgument as? EnumInst,
              let sourcePayload = sourceEnum.payload,
              let (se, someCase) = isPayloadOfSwitchEnum(sourcePayload),
              enumInst.caseIndex == someCase,
              sourceEnum.caseIndex == someCase,
              sourceEnum.type == se.enumOp.type else {
          return nil
        }
        someSwitch = se
      } else {
        guard let (se, someCase) = isPayloadOfSwitchEnum(callArgument),
              enumInst.caseIndex == someCase else {
          return nil
        }
        someSwitch = se
      }
    } else {
      // The none-case
      if noneSwitch != nil { return nil }
      guard let singlePred = enumInst.parentBlock.singlePredecessor,
            let se = singlePred.terminator as? SwitchEnumInst,
            se.getUniqueSuccessor(forCaseIndex: enumInst.caseIndex) === enumInst.parentBlock else {
        return nil
      }
      noneSwitch = se
    }
  }
  guard let noneSwitch = noneSwitch,
        let someSwitch = someSwitch,
        noneSwitch == someSwitch else {
    return nil
  }
  return someSwitch
}

/// Returns the `switch_enum` together with the enum case index, if `value` is
/// the payload block argument of the `switch_enum`.
private func isPayloadOfSwitchEnum(_ value: Value) -> (SwitchEnumInst, case: Int)? {
  if let payloadArg = value as? BlockArgument,
     let pred = payloadArg.parentBlock.singlePredecessor,
     let se = pred.terminator as? SwitchEnumInst,
     let caseIdx = se.getUniqueCase(forSuccessor: payloadArg.parentBlock) {
    return (se, caseIdx)
  }
  return nil
}

/// Returns the apply instruction if `value` is an ObjC -> Swift bridging call.
func isBridgeToSwiftCall(_ value: Value) -> ApplyInst? {
  guard let bridgingCall = value as? ApplyInst,
        let bridgingFunc = bridgingCall.referencedFunction else {
    return nil
  }
  let funcName = bridgingFunc.name
  guard  bridgingFunc.hasSemanticsAttribute("bridgeFromObjectiveC") ||
         // Currently the semantics attribute is not used, so test for specific functions, too.
         // TODO: remove those checks once the briding functions are annotate with "bridgeFromObjectiveC"
         //       in Foundation.
         //
         // String._unconditionallyBridgeFromObjectiveC(_:)
         funcName == "$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ" ||
         // Array._unconditionallyBridgeFromObjectiveC(_:)
         funcName == "$sSa10FoundationE36_unconditionallyBridgeFromObjectiveCySayxGSo7NSArrayCSgFZ" else {
    return nil
  }
  guard bridgingCall.arguments.count == 2,
        bridgingCall.getArgumentConvention(calleeArgIndex: 0) == .directGuaranteed else {
    return nil
  }
  return bridgingCall
}

/// Returns the apply instruction if `value` is a Swift -> ObjC bridging call.
func isBridgeToObjcCall(_ value: Value) -> ApplyInst? {
  guard let bridgingCall = value as? ApplyInst,
        let bridgingFunc = bridgingCall.referencedFunction,
        bridgingFunc.hasSemanticsAttribute("convertToObjectiveC"),
        bridgingCall.arguments.count == 1,
        bridgingCall.getArgumentConvention(calleeArgIndex: 0) == .directGuaranteed else {
    return nil
  }
  return bridgingCall
}
