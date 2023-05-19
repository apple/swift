//===--- SimplifyPartialApply.swift ---------------------------------------===//
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

extension PartialApplyInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    let optimizedApplyOfPartialApply = context.tryOptimizeApplyOfPartialApply(closure: self)
    if optimizedApplyOfPartialApply {
      context.notifyInvalidatedStackNesting()
    }

    if context.preserveDebugInfo && uses.contains(where: { $0.instruction is DebugValueInst }) {
      return
    }

    // Try to delete the partial_apply.
    // In case it became dead because of tryOptimizeApplyOfPartialApply, we don't
    // need to copy all arguments again (to extend their lifetimes), because it
    // was already done in tryOptimizeApplyOfPartialApply.
    if context.tryDeleteDeadClosure(closure: self, needKeepArgsAlive: !optimizedApplyOfPartialApply) {
      context.notifyInvalidatedStackNesting()
    }
  }
}
