//===--- Options.swift ----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL
import OptimizerBridging

struct Options {
  let _bridged: BridgedPassContext

  var enableStackProtection: Bool {
    _bridged.enableStackProtection()
  }

  var enableMoveInoutStackProtection: Bool {
    _bridged.enableMoveInoutStackProtection()
  }

  func enableSimplification(for inst: Instruction) -> Bool {
    _bridged.enableSimplificationFor(inst.bridged)
  }

  enum AssertConfiguration {
    case enabled
    case disabled
    case unknown
  }

  var assertConfiguration: AssertConfiguration {
    switch _bridged.getAssertConfiguration() {
      case .Debug:               return .enabled
      case .Release, .Unchecked: return .disabled
      default:                   return .unknown
    }
  }
}
