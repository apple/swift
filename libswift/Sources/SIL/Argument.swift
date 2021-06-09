//===--- Argument.swift - Defines the Argument classes --------------===//
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

import SILBridging

/// A basic block argument.
///
/// Maps to both, SILPhiArgument and SILFunctionArgument.
public class BlockArgument : Value {
  final public var definingInstruction: Instruction? { nil }

  final public var block: BasicBlock {
    return SILArgument_getParent(bridged).block
  }

  var bridged: BridgedArgument { BridgedArgument(obj: SwiftObject(self)) }
}

// Bridging utilities

extension BridgedArgument {
  var argument: BlockArgument { obj.getAs(BlockArgument.self) }
}

