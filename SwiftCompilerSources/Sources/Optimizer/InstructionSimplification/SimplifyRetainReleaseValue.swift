//===--- SimplifyRetainReleaseValue.swift ---------------------------------===//
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

extension RetainValueInst : Simplifyable, SILCombineSimplifyable {
  func simplify(_ context: SimplifyContext) {

    // Remove pairs of
    // ```
    //   release_value %0   // the release is before the retain!
    //   retain_value %0
    // ```
    // which sometimes the ARC optimizations cannot do.
    //
    if optimizeReleaseRetainPair(context) {
      return
    }

    // Replace
    // ```
    //   %1 = enum #E.A, %0
    //   retain_value %1
    // ```
    // with
    // ```
    //   %1 = enum #E.A, %0   // maybe dead
    //   retain_value %0
    // ```
    replaceOperandWithPayloadOfEnum(context)

    // Remove if the operand is trivial (but not necessarily its type), e.g.
    // ```
    //   %1 = value_to_bridge_object %0 : $UInt64
    //   retain_value %1 : $Builtin.BridgeObject
    // ```
    if removeIfOperandIsTrivial(context) {
      return
    }

    // Replace e.g.
    // ```
    //   retain_value %1 : $SomeClass
    // ```
    // with
    // ```
    //   strong_retain %1 : $SomeClass
    // ```
    replaceWithStrongOrUnownedRetain(context)
  }
}

extension ReleaseValueInst : Simplifyable, SILCombineSimplifyable {
  func simplify(_ context: SimplifyContext) {

    // Replace
    // ```
    //   %1 = enum #E.A, %0
    //   release_value %1
    // ```
    // with
    // ```
    //   %1 = enum #E.A, %0   // maybe dead
    //   release_value %0
    // ```
    replaceOperandWithPayloadOfEnum(context)

    // Remove if the operand is trivial (but not necessarily its type), e.g.
    // ```
    //   %1 = value_to_bridge_object %0 : $UInt64
    //   release_value %1 : $Builtin.BridgeObject
    // ```
    if removeIfOperandIsTrivial(context) {
      return
    }

    // Replace e.g.
    // ```
    //   release_value %1 : $SomeClass
    // ```
    // with
    // ```
    //   release_value %1 : $SomeClass
    // ```
    replaceWithStrongOrUnownedRelease(context)
  }
}

private extension RetainValueInst {
  func optimizeReleaseRetainPair(_ context: SimplifyContext) -> Bool {
    if let prevInst = self.previous,
       let release = prevInst as? ReleaseValueInst,
       release.value == self.value {
      context.erase(instruction: release)
      context.erase(instruction: self)
      return true
    }
    return false
  }

  func replaceWithStrongOrUnownedRetain(_ context: SimplifyContext) {
    if value.type.isReferenceCounted(in: parentFunction) {
      let builder = Builder(before: self, context)
      if value.type.isUnownedStorageType {
        builder.createUnownedRetain(operand: value)
      } else {
        builder.createStrongRetain(operand: value)
      }
      context.erase(instruction: self)
    }
  }
}

private extension ReleaseValueInst {
  func replaceWithStrongOrUnownedRelease(_ context: SimplifyContext) {
    if value.type.isReferenceCounted(in: parentFunction) {
      let builder = Builder(before: self, context)
      if value.type.isUnownedStorageType {
        builder.createUnownedRelease(operand: value)
      } else {
        builder.createStrongRelease(operand: value)
      }
      context.erase(instruction: self)
    }
  }
}

private extension UnaryInstruction {
  func replaceOperandWithPayloadOfEnum(_ context: SimplifyContext) {
    if let e = operand.value as? EnumInst,
       !e.type.isValueTypeWithDeinit,
       let payload = e.payload {
      operand.set(to: payload, context)
    }
  }

  func removeIfOperandIsTrivial(_ context: SimplifyContext) -> Bool {
    if operand.value.isTrivial(context) {
      context.erase(instruction: self)
      return true
    }
    return false
  }
}
