//===--- PassRegistration.swift - Register optimization passes -------------===//
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
import Parse

@_cdecl("initializeSwiftModules")
public func initializeSwiftModules() {
  registerSILClasses()
  registerSwiftAnalyses()
  registerSwiftPasses()
  initializeSwiftParseModules()
}

private func registerPass(
      _ pass: ModulePass,
      _ runFn: @escaping (@convention(c) (BridgedPassContext) -> ())) {
  pass.name._withStringRef { nameStr in
    SILPassManager_registerModulePass(nameStr, runFn)
  }
}

private func registerPass(
      _ pass: FunctionPass,
      _ runFn: @escaping (@convention(c) (BridgedFunctionPassCtxt) -> ())) {
  pass.name._withStringRef { nameStr in
    SILPassManager_registerFunctionPass(nameStr, runFn)
  }
}

protocol SILCombineSimplifyable : Instruction {
  func simplify(_ context: SimplifyContext)
}

private func run<InstType: SILCombineSimplifyable>(_ instType: InstType.Type,
                                                   _ bridgedCtxt: BridgedInstructionPassCtxt) {
  let inst = bridgedCtxt.instruction.getAs(instType)
  let context = SimplifyContext(_bridged: bridgedCtxt.passContext,
                                notifyInstructionChanged: {inst in},
                                preserveDebugInfo: false)
  inst.simplify(context)
}

private func registerForSILCombine<InstType: SILCombineSimplifyable>(
      _ instType: InstType.Type,
      _ runFn: @escaping (@convention(c) (BridgedInstructionPassCtxt) -> ())) {
  String(describing: instType)._withStringRef { instClassStr in
    SILCombine_registerInstructionPass(instClassStr, runFn)
  }
}

private func registerSwiftPasses() {
  // Module passes
  registerPass(mandatoryPerformanceOptimizations, { mandatoryPerformanceOptimizations.run($0) })
  registerPass(readOnlyGlobalVariablesPass, { readOnlyGlobalVariablesPass.run($0) })
  registerPass(stackProtection, { stackProtection.run($0) })

  // Function passes
  registerPass(mergeCondFailsPass, { mergeCondFailsPass.run($0) })
  registerPass(computeEscapeEffects, { computeEscapeEffects.run($0) })
  registerPass(computeSideEffects, { computeSideEffects.run($0) })
  registerPass(initializeStaticGlobalsPass, { initializeStaticGlobalsPass.run($0) })
  registerPass(objCBridgingOptimization, { objCBridgingOptimization.run($0) })
  registerPass(objectOutliner, { objectOutliner.run($0) })
  registerPass(stackPromotion, { stackPromotion.run($0) })
  registerPass(functionStackProtection, { functionStackProtection.run($0) })
  registerPass(assumeSingleThreadedPass, { assumeSingleThreadedPass.run($0) })
  registerPass(releaseDevirtualizerPass, { releaseDevirtualizerPass.run($0) })
  registerPass(simplificationPass, { simplificationPass.run($0) })
  registerPass(ononeSimplificationPass, { ononeSimplificationPass.run($0) })
  registerPass(lateOnoneSimplificationPass, { lateOnoneSimplificationPass.run($0) })
  registerPass(cleanupDebugStepsPass, { cleanupDebugStepsPass.run($0) })
  registerPass(namedReturnValueOptimization, { namedReturnValueOptimization.run($0) })

  // Instruction passes
  registerForSILCombine(BeginCOWMutationInst.self, { run(BeginCOWMutationInst.self, $0) })
  registerForSILCombine(GlobalValueInst.self,      { run(GlobalValueInst.self, $0) })
  registerForSILCombine(StrongRetainInst.self,     { run(StrongRetainInst.self, $0) })
  registerForSILCombine(StrongReleaseInst.self,    { run(StrongReleaseInst.self, $0) })
  registerForSILCombine(RetainValueInst.self,      { run(RetainValueInst.self, $0) })
  registerForSILCombine(ReleaseValueInst.self,     { run(ReleaseValueInst.self, $0) })
  registerForSILCombine(LoadInst.self,             { run(LoadInst.self, $0) })

  // Test passes
  registerPass(functionUsesDumper, { functionUsesDumper.run($0) })
  registerPass(silPrinterPass, { silPrinterPass.run($0) })
  registerPass(escapeInfoDumper, { escapeInfoDumper.run($0) })
  registerPass(addressEscapeInfoDumper, { addressEscapeInfoDumper.run($0) })
  registerPass(accessDumper, { accessDumper.run($0) })
  registerPass(deadEndBlockDumper, { deadEndBlockDumper.run($0) })
  registerPass(memBehaviorDumper, { memBehaviorDumper.run($0) })
  registerPass(rangeDumper, { rangeDumper.run($0) })
  registerPass(runUnitTests, { runUnitTests.run($0) })
  registerPass(testInstructionIteration, { testInstructionIteration.run($0) })
}

private func registerSwiftAnalyses() {
  AliasAnalysis.register()
  CalleeAnalysis.register()
}
