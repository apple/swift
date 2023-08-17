//===--- SimplifyPointerToAddress.swift -----------------------------------===//
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

extension PointerToAddressInst : OnoneSimplifyable {

  /// For a redundant pair of pointer-address conversions, e.g.
  ///
  ///   %2 = address_to_pointer %1
  ///   %3 = pointer_to_address %2 [strict]
  ///
  /// replace all uses of %3 with %1.
  ///
  func simplify(_ context: SimplifyContext) {
    if let atp = self.pointer as? AddressToPointerInst,
       atp.address.type == self.type,
       self.isStrict,

       // If the pointer is within an ownership scope, the transformation can break ownership rules, e.g.
       //   %2 = begin_borrow %1
       //   %3 = ref_tail_addr %2
       //   %4 = address_to_pointer %3
       //   end_borrow %2
       //   %5 = pointer_to_address %4             <- cannot replace %5 with %3!
       //
       !atp.address.accessBase.hasLocalOwnershipLifetime
    {
      self.uses.replaceAll(with: atp.address, context)
    }
  }
}
