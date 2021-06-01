//===--- GlobalVariable.swift - Defines the GlobalVariable class ----------===//
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

final public class GlobalVariable : CustomStringConvertible {
  public var name: String {
    return SILGlobalVariable_getName(bridged).string
  }

  public var description: String {
    return SILGlobalVariable_debugDescription(bridged).takeString()
  }

  // TODO: initializer instructions

  var bridged: BridgedGlobalVar { BridgedGlobalVar(obj: SwiftObject(self)) }
}

// Bridging utilities

extension BridgedGlobalVar {
  var globalVar: GlobalVariable { obj.getAs(GlobalVariable.self) }
}
