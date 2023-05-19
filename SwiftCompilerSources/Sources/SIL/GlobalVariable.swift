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

import Basic
import SILBridging

final public class GlobalVariable : CustomStringConvertible, HasShortDescription, Hashable {
  public var name: StringRef {
    return StringRef(bridged: bridged.getName())
  }

  public var description: String {
    let stdString = bridged.getDebugDescription()
    return String(_cxxString: stdString)
  }

  public var shortDescription: String { name.string }

  public var isLet: Bool { bridged.isLet() }

  /// True, if the linkage of the global variable indicates that it is visible outside the current
  /// compilation unit and therefore not all of its uses are known.
  ///
  /// For example, `public` linkage.
  public var isPossiblyUsedExternally: Bool {
    return bridged.isPossiblyUsedExternally()
  }

  public var staticInitValue: SingleValueInstruction? {
    bridged.getStaticInitializerValue().instruction as? SingleValueInstruction
  }

  /// True if the global's linkage and resilience expansion allow the global
  /// to be initialized statically.
  public var canBeInitializedStatically: Bool {
    return bridged.canBeInitializedStatically()
  }

  public static func ==(lhs: GlobalVariable, rhs: GlobalVariable) -> Bool {
    lhs === rhs
  }

  public func hash(into hasher: inout Hasher) {
    hasher.combine(ObjectIdentifier(self))
  }

  public var bridged: BridgedGlobalVar { BridgedGlobalVar(obj: SwiftObject(self)) }
}

extension Instruction {
  public var isValidInStaticInitializerOfGlobal: Bool {
    return BridgedGlobalVar.isValidStaticInitializer(bridged)
  }
}

// Bridging utilities

extension BridgedGlobalVar {
  var globalVar: GlobalVariable { obj.getAs(GlobalVariable.self) }
}

extension OptionalBridgedGlobalVar {
  public var globalVar: GlobalVariable? { obj.getAs(GlobalVariable.self) }
}
