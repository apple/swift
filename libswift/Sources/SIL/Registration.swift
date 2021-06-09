//===--- Registration.swift - register SIL classes ------------------------===//
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

private func register<T: AnyObject>(_ cl: T.Type) {
  String(describing: cl).withBridgedStringRef { nameStr in
    let metatype = unsafeBitCast(cl, to: SwiftMetatype.self)
    registerBridgedClass(nameStr, metatype)
  }
}

public func registerSILClasses() {
  register(Function.self)
  register(BasicBlock.self)
  register(GlobalVariable.self)

  // The "unimplemented" registrations must be done before all other node
  // registrations. In the order from super -> sub class.
  register(UnimplementedInstruction.self)
  register(UnimplementedSingleValueInst.self)
  register(UnimplementedRefCountingInst.self)
  register(MultipleValueInstructionResult.self)
  register(UnimplementedMultiValueInst.self)

  register(Undef.self)
  register(PlaceholderValue.self)

  register(BlockArgument.self)

  register(CondFailInst.self)
  register(DebugValueInst.self)
  register(BuiltinInst.self)
  register(UpcastInst.self)
  register(GlobalAddrInst.self)
  register(GlobalValueInst.self)
  register(TupleExtractInst.self)
  register(RefElementAddrInst.self)
  register(RefTailAddrInst.self)
}
