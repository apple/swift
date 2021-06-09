//===--- PassRegistration.swift - Register optimzation passes -------------===//
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

import SIL
import OptimizerBridging

@_cdecl("initializeLibSwift")
public func initializeLibSwift() {
  registerSILClasses()
  registerSwiftPasses()
}

private func registerPass(
      _ pass: FunctionPass,
      _ runFn: @escaping (@convention(c) (BridgedFunctionPassCtxt) -> ())) {
  pass.name.withBridgedStringRef { nameStr in
    SILPassManager_registerFunctionPass(nameStr, runFn)
  }
}

private func registerPass<InstType: Instruction>(
      _ pass: InstructionPass<InstType>,
      _ runFn: @escaping (@convention(c) (BridgedInstructionPassCtxt) -> ())) {
  pass.name.withBridgedStringRef { nameStr in
    SILCombine_registerInstructionPass(nameStr, runFn)
  }
}

private func registerSwiftPasses() {
  registerPass(silPrinterPass, { silPrinterPass.run($0) })
  registerPass(mergeCondFailsPass, { mergeCondFailsPass.run($0) })
  registerPass(simplifyGlobalValuePass, { simplifyGlobalValuePass.run($0) })
}
