//===--- SimplifyValueToBridgeObject.swift --------------------------------===//
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

extension ValueToBridgeObjectInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {

    // Optimize the sequence
    // ```
    //   %1 = value_to_bridge_object %0
    //   %2 = struct $SomeInt (%1)
    //   %3 = unchecked_trivial_bit_cast %2
    //   %4 = struct_extract %3, #valueFieldInInt
    //   %5 = value_to_bridge_object %4
    // ```
    // to
    // ```
    //   %5 = value_to_bridge_object %0
    // ```
    // This sequence comes up in the code for constructing an empty string literal.
    //
    if let se = self.value as? StructExtractInst,
       let utbc = se.struct as? UncheckedTrivialBitCastInst,
       let vtbo = utbc.fromValue as? ValueToBridgeObjectInst {
      self.operand.set(to: vtbo.value, context)
    }
  }
}
