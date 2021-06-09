//===--- PassUtils.swift - Utilities for optimzation passes ---------------===//
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

public typealias BridgedFunctionPassCtxt =
  OptimizerBridging.BridgedFunctionPassCtxt
public typealias BridgedInstructionPassCtxt =
  OptimizerBridging.BridgedInstructionPassCtxt

struct FunctionPassContext {

  fileprivate let passContext: BridgedPassContext
  fileprivate let function: Function
  
  func erase(instruction: Instruction) {
    PassContext_eraseInstruction(passContext, instruction.bridged)
  }
  
  private func notifyChanges(_ kind: ChangeNotificationKind) {
    PassContext_notifyChanges(passContext, kind)
  }
}

struct FunctionPass {

  let name: String
  let runFunction: (Function, FunctionPassContext) -> ()

  public init(name: String,
              _ runFunction: @escaping (Function, FunctionPassContext) -> ()) {
    self.name = name
    self.runFunction = runFunction
  }

  func run(_ bridgedCtxt: BridgedFunctionPassCtxt) {
    let function = bridgedCtxt.function.function
    let context = FunctionPassContext(passContext: bridgedCtxt.passContext,
                                      function: function)
    runFunction(function, context)
  }
}

struct InstructionPassContext {
  fileprivate let passContext: BridgedPassContext

  func erase(instruction: Instruction) {
    PassContext_eraseInstruction(passContext, instruction.bridged)
  }

  private func notifyChanges(_ kind: ChangeNotificationKind) {
    PassContext_notifyChanges(passContext, kind)
  }
}

struct InstructionPass<InstType: Instruction> {

  let name: String
  let runFunction: (InstType, InstructionPassContext) -> ()

  public init(name: String, _ runFunction: @escaping (InstType, InstructionPassContext) -> ()) {
    self.name = name
    self.runFunction = runFunction
  }

  func run(_ bridgedCtxt: BridgedInstructionPassCtxt) {
    let inst = bridgedCtxt.instruction.getAs(InstType.self)
    let context = InstructionPassContext(passContext: bridgedCtxt.passContext)
    runFunction(inst, context)
  }
}

extension StackList {
  init(_ context: FunctionPassContext) {
    self.init(context: context.passContext)
  }
  
  init(_ context: InstructionPassContext) {
    self.init(context: context.passContext)
  }
}

extension Builder {
  init(at insPnt: Instruction, location: Location,
       _ context: FunctionPassContext) {
    self.init(insertionPoint: insPnt, location: location,
              passContext: context.passContext)
  }

  init(at insPnt: Instruction, _ context: FunctionPassContext) {
    self.init(insertionPoint: insPnt, location: insPnt.location,
              passContext: context.passContext)
  }

  init(at insPnt: Instruction, location: Location,
       _ context: InstructionPassContext) {
    self.init(insertionPoint: insPnt, location: location,
              passContext: context.passContext)
  }

  init(at insPnt: Instruction, _ context: InstructionPassContext) {
    self.init(insertionPoint: insPnt, location: insPnt.location,
              passContext: context.passContext)
  }
}
